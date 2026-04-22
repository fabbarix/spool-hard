#include "printer_ftp.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

PrinterFtp::PrinterFtp() {
    _control.setInsecure();  // Bambu uses a self-signed cert on :990
}
PrinterFtp::~PrinterFtp() { quit(); }

void PrinterFtp::_emit(const char* step, int code, const String& text) {
    if (!_trace) return;
    uint32_t t = _traceStart ? (millis() - _traceStart) : 0;
    _trace(step, code, text, t);
}

int PrinterFtp::_readResponse(String* body, uint32_t timeout_ms) {
    // FTP response: "<3-digit code><space-or-dash><text>\r\n", possibly spanning
    // multiple lines when the code is followed by '-'. Drain until we see the
    // same code followed by a space.
    uint32_t start = millis();
    String line;
    int code = -1;
    bool multiline = false;
    String multiCode;
    uint32_t last_probe = 0;

    uint32_t iters = 0;
    while (millis() - start < timeout_ms) {
        int avail = _control.available();
        ++iters;
        if (millis() - last_probe > 1000) {
            last_probe = millis();
            Serial.printf("[FTP] wait %lums  avail=%d  connected=%d  iters=%u\n",
                          (unsigned long)(millis() - start),
                          avail,
                          _control.connected() ? 1 : 0,
                          (unsigned)iters);
        }
        if (avail <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Batch read so the TLS layer processes the record in one call and
        // resets its read-pointer cleanly — individual byte reads left
        // in_offt state pointing mid-record and subsequent available() polls
        // didn't fetch fresh records.
        uint8_t tbuf[128];
        int got = _control.read(tbuf, avail > (int)sizeof(tbuf) ? (int)sizeof(tbuf) : avail);
        if (got <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        for (int idx = 0; idx < got; ++idx) {
            char c = (char)tbuf[idx];
            if (c == '\n') {
                // Do NOT String::trim() — it strips the trailing space
                // that separates "NNN " from the message text, which is
                // what line[3] below checks for. Bambu's "331 " (no text
                // after the space) would otherwise collapse to "331" and
                // fail the line.length() >= 4 guard, leading to a 5 s
                // timeout with `text=331 code=-1`. Any trailing \r was
                // already filtered out by the else branch below.
                if (body) { if (body->length()) *body += '\n'; *body += line; }
                if (line.length() >= 4 && isdigit(line[0]) && isdigit(line[1]) && isdigit(line[2])) {
                    int thisCode = atoi(line.substring(0, 3).c_str());
                    if (line[3] == '-' && !multiline) {
                        multiline = true;
                        multiCode = line.substring(0, 3);
                        code = thisCode;
                    } else if (line[3] == ' ') {
                        // Either a terminating line of a multi-line, or a single-line.
                        if (!multiline || line.substring(0, 3) == multiCode) {
                            return thisCode;
                        }
                    }
                }
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
        delay(5);
    }
    _lastError = "response timeout";
    return -1;
}

int PrinterFtp::_sendCmd(const String& cmd) {
    // SINGLE write so mbedtls wraps the whole FTP command + CRLF in ONE TLS
    // application-data record. Bambu's FTPS server waits indefinitely for a
    // CRLF in the SAME record as the verb — splitting "USER bblp" and "\r\n"
    // across two records (the natural result of two _control.print() calls)
    // was producing a silent 5-second timeout after the 220 banner. Matches
    // how the Rust reference does it (single session.write + session.flush).
    String line = cmd + "\r\n";
    Serial.printf("[FTP] > %s\n", cmd.c_str());
    _control.write((const uint8_t*)line.c_str(), line.length());
    String body;
    int code = _readResponse(&body);
    // Redact passwords in the trace — PASS's argument is the printer's
    // access code, which we don't want showing up in the web UI.
    String label = cmd.startsWith("PASS") ? String("PASS ********") : cmd;
    _emit(label.c_str(), code, body);
    return code;
}

bool PrinterFtp::connect(const IPAddress& ip, const String& access_code,
                         const String& sni_hostname, uint16_t port) {
    _ip  = ip;
    _sni = sni_hostname;
    _emit("connect", 0,
          String("tls://") + ip.toString() + ":" + String(port)
            + " sni=" + sni_hostname);
    Serial.printf("[FTP] Connecting to %s:%u (implicit TLS, SNI=%s)  internal-heap=%u (largest %u)\n",
                  ip.toString().c_str(), port, sni_hostname.c_str(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const char* sni_cstr = sni_hostname.length() ? sni_hostname.c_str() : nullptr;
    if (!_control.connect(ip, port, sni_cstr, nullptr, nullptr, nullptr)) {
        char errBuf[128] = {0};
        int lastErr = _control.lastError(errBuf, sizeof(errBuf));
        _lastError = String("TCP/TLS connect failed");
        if (lastErr != 0) {
            _lastError += " (mbedtls ";
            _lastError += String(lastErr, HEX);
            _lastError += ": ";
            _lastError += errBuf;
            _lastError += ")";
        }
        Serial.printf("[FTP] connect fail: %s\n", _lastError.c_str());
        _emit("tls handshake", -1, _lastError);
        return false;
    }
    _emit("tls handshake", 0, "ok");
    // Read the banner (220).
    String bannerBody;
    int banner = _readResponse(&bannerBody);
    _emit("banner", banner, bannerBody);
    if (banner != 220) {
        _lastError = "no 220 banner (got " + String(banner) + ")";
        _control.stop();
        return false;
    }
    int rc = _sendCmd("USER bblp");
    if (rc != 331) {
        _lastError = "USER rejected (got " + String(rc) + ")";
        Serial.printf("[FTP] USER bblp → %d\n", rc);
        _control.stop();
        return false;
    }
    rc = _sendCmd("PASS " + access_code);
    if (rc != 230) {
        _lastError = "PASS rejected (got " + String(rc) + ", wrong access code?)";
        Serial.printf("[FTP] PASS → %d\n", rc);
        _control.stop();
        return false;
    }
    rc = _sendCmd("TYPE I");
    if (rc != 200) {
        _lastError = "TYPE I rejected (got " + String(rc) + ")";
        _control.stop();
        return false;
    }
    Serial.println("[FTP] Authenticated");
    return true;
}

bool PrinterFtp::quit() {
    if (_control.connected()) {
        _control.print("QUIT\r\n");
        _readResponse(nullptr, 1000);
        _control.stop();
    }
    return true;
}

bool PrinterFtp::_parsePasv(const String& line, IPAddress& ip, uint16_t& port) {
    int open  = line.indexOf('(');
    int close = line.indexOf(')');
    if (open < 0 || close <= open) return false;
    String nums = line.substring(open + 1, close);
    int values[6] = {0};
    int idx = 0;
    int start = 0;
    for (int i = 0; i <= (int)nums.length(); ++i) {
        if (i == (int)nums.length() || nums[i] == ',') {
            if (idx >= 6) return false;
            values[idx++] = atoi(nums.substring(start, i).c_str());
            start = i + 1;
        }
    }
    if (idx != 6) return false;
    ip   = IPAddress(values[0], values[1], values[2], values[3]);
    port = (uint16_t)((values[4] << 8) | values[5]);
    return true;
}

bool PrinterFtp::_openDataAndRetrieve(const String& path, uint32_t offset,
                                      std::function<bool(WiFiClientSecure&)> reader) {
    if (!_control.connected()) { _lastError = "control closed"; return false; }

    // PASV to get the data endpoint.
    _control.print("PASV\r\n");
    String pasvLine;
    int code = _readResponse(&pasvLine);
    if (code != 227) { _lastError = "PASV failed"; return false; }
    IPAddress dip; uint16_t dport;
    if (!_parsePasv(pasvLine, dip, dport)) { _lastError = "PASV parse failed"; return false; }

    // REST <offset> for range requests. Silent failure is fine — server
    // returns 350 if accepted. If it doesn't support it we fall back to
    // consuming from the start.
    bool did_rest = false;
    if (offset > 0) {
        char buf[32]; snprintf(buf, sizeof(buf), "REST %u", offset);
        if (_sendCmd(buf) == 350) did_rest = true;
    }

    // Open the data connection — also TLS with the same SNI (see control
    // connect notes). Skipping SNI here made the data TCP accept but then
    // silently drop app traffic, same symptom as the control channel.
    WiFiClientSecure data;
    data.setInsecure();
    const char* data_sni = _sni.length() ? _sni.c_str() : nullptr;
    if (!data.connect(dip, dport, data_sni, nullptr, nullptr, nullptr)) {
        _lastError = "data TCP connect failed";
        return false;
    }

    // RETR kicks off the transfer. Server replies 150 on control. Build the
    // whole line first so mbedtls emits a single TLS record (see _sendCmd).
    {
        String retr = "RETR " + path + "\r\n";
        _control.write((const uint8_t*)retr.c_str(), retr.length());
    }
    int retr_code = _readResponse();
    if (retr_code != 150 && retr_code != 125) {
        _lastError = "RETR rejected";
        data.stop();
        return false;
    }

    // If REST didn't take, burn `offset` bytes.
    if (!did_rest && offset > 0) {
        uint32_t remaining = offset;
        uint8_t scratch[512];
        uint32_t start = millis();
        while (remaining > 0 && (data.connected() || data.available()) &&
               millis() - start < 30000) {
            size_t n = data.read(scratch, remaining > sizeof(scratch) ? sizeof(scratch) : remaining);
            if (n > 0) { remaining -= n; start = millis(); }
            else       delay(5);
        }
    }

    // Stream through user callback.
    bool ok = reader(data);
    data.stop();

    // Drain control completion (226).
    _readResponse();
    return ok;
}

bool PrinterFtp::retrieveRange(const String& path, uint32_t offset, uint32_t length, RangeCb cb) {
    uint32_t read_total = 0;
    return _openDataAndRetrieve(path, offset, [&](WiFiClientSecure& d) {
        uint8_t buf[1024];
        uint32_t start = millis();
        while (read_total < length && (d.connected() || d.available()) &&
               millis() - start < 30000) {
            size_t want = length - read_total;
            if (want > sizeof(buf)) want = sizeof(buf);
            int n = d.read(buf, want);
            if (n > 0) {
                if (!cb(buf, (size_t)n)) return false;
                read_total += n;
                start = millis();
            } else delay(5);
        }
        return read_total == length;
    });
}

bool PrinterFtp::retrieveInto(const String& path, uint32_t offset, uint32_t length, uint8_t* dst) {
    size_t pos = 0;
    bool ok = retrieveRange(path, offset, length, [&](const uint8_t* d, size_t n) {
        if (pos + n > length) return false;
        memcpy(dst + pos, d, n);
        pos += n;
        return true;
    });
    return ok && pos == length;
}

bool PrinterFtp::retrieveStream(const String& path, ChunkCb cb) {
    size_t total = 0;
    return _openDataAndRetrieve(path, 0, [&](WiFiClientSecure& d) {
        uint8_t buf[1024];
        uint32_t start = millis();
        while ((d.connected() || d.available()) && millis() - start < 60000) {
            int n = d.read(buf, sizeof(buf));
            if (n > 0) {
                total += n;
                if (!cb(buf, (size_t)n, total)) return false;
                start = millis();
            } else delay(5);
        }
        return true;
    });
}

int32_t PrinterFtp::size(const String& path) {
    _control.print("SIZE "); _control.print(path); _control.print("\r\n");
    String body;
    int code = _readResponse(&body);
    if (code != 213) return -1;
    // "213 12345"
    int sp = body.indexOf(' ');
    if (sp < 0) return -1;
    return body.substring(sp + 1).toInt();
}

bool PrinterFtp::listDir(const String& path, std::vector<String>& out) {
    out.clear();
    if (!_control.connected()) { _lastError = "control closed"; return false; }

    // PASV to get data endpoint — same flow as _openDataAndRetrieve but
    // we issue LIST instead of RETR and collect server lines verbatim.
    _control.print("PASV\r\n");
    String pasvBody;
    int code = _readResponse(&pasvBody);
    _emit("PASV", code, pasvBody);
    if (code != 227) { _lastError = "PASV failed"; return false; }
    IPAddress dip; uint16_t dport;
    if (!_parsePasv(pasvBody, dip, dport)) {
        _lastError = "PASV parse failed";
        _emit("PASV parse", -1, pasvBody);
        return false;
    }

    WiFiClientSecure data;
    data.setInsecure();
    const char* data_sni = _sni.length() ? _sni.c_str() : nullptr;
    if (!data.connect(dip, dport, data_sni, nullptr, nullptr, nullptr)) {
        _lastError = "data TCP connect failed";
        _emit("data connect", -1, _lastError);
        return false;
    }
    _emit("data connect", 0, dip.toString() + ":" + String(dport));

    // LIST — same single-record discipline as _sendCmd; Bambu's FTPS
    // silently hangs otherwise.
    {
        String cmd = "LIST " + path + "\r\n";
        _control.write((const uint8_t*)cmd.c_str(), cmd.length());
    }
    String body;
    int list_code = _readResponse(&body);
    _emit(("LIST " + path).c_str(), list_code, body);
    if (list_code != 150 && list_code != 125) {
        _lastError = "LIST rejected (got " + String(list_code) + ")";
        data.stop();
        return false;
    }

    // Drain the data channel — server closes it when the listing is done.
    String line;
    uint32_t start = millis();
    while ((data.connected() || data.available()) && millis() - start < 15000) {
        int avail = data.available();
        if (avail <= 0) { delay(10); continue; }
        while (avail-- > 0) {
            int c_in = data.read();
            if (c_in < 0) break;
            char c = (char)c_in;
            if (c == '\n') {
                // Strip trailing CR.
                if (line.length() && line[line.length() - 1] == '\r') line.remove(line.length() - 1);
                if (line.length()) out.push_back(line);
                line = "";
            } else {
                line += c;
            }
        }
        start = millis();
    }
    if (line.length()) out.push_back(line);
    data.stop();

    // 226 on control to confirm transfer complete.
    String endBody;
    int end_code = _readResponse(&endBody);
    _emit("LIST complete", end_code, endBody);

    _emit("listing", 0, String(out.size()) + " entr" + (out.size() == 1 ? "y" : "ies"));
    return true;
}
