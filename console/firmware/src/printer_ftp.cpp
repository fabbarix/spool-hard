#include "printer_ftp.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <algorithm>      // std::rotate
#include <ctype.h>        // isdigit

// Internal helper — convert mbedtls error code to a short human string we
// can stash in _lastError. mbedtls_strerror needs MBEDTLS_ERROR_C in the
// build; arduino-esp32's mbedtls config has it, so we use the real one.
#include <mbedtls/error.h>
#include <mbedtls/platform.h>

// PSRAM-preferring calloc/free for mbedtls. Arduino-esp32's mbedtls 2.28
// pre-allocates ~32 KB of in+out buffers per ssl_context at setup time,
// and the FTPS session-reuse pattern needs control AND data contexts
// live simultaneously — that's ~64 KB of contiguous internal DRAM,
// which the ESP32-S3 just doesn't have free mid-session. PSRAM has
// nearly 2 MB free, so we route calloc through there. AES ops in PSRAM
// are slower (~4-5×) but FTPS payloads are bulk-copied, not crypto-
// hot, and the alternative (recompile mbedtls with VARIABLE_BUFFER) is
// out of reach from a sketch. Override is global; installed once and
// left in place — other mbedtls users (MQTT, OTA HTTPS, cloud sync)
// inherit the same allocator without behavioural change beyond
// throughput.
static void* _psram_calloc(size_t n, size_t size) {
    size_t bytes = n * size;
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);  // fallback DRAM
    (void)bytes;
    return p;
}
static void _psram_free(void* p) { heap_caps_free(p); }
static bool _mbed_alloc_overridden = false;
static void _install_psram_alloc_once() {
    if (_mbed_alloc_overridden) return;
    mbedtls_platform_set_calloc_free(_psram_calloc, _psram_free);
    _mbed_alloc_overridden = true;
}

static String _mbedErr(int rc) {
    char buf[96] = {0};
    mbedtls_strerror(rc, buf, sizeof(buf));
    char hex[16];
    snprintf(hex, sizeof(hex), "-0x%04X", -rc);
    return String(hex) + " " + buf;
}

// ─────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────

PrinterFtp::PrinterFtp() {
    mbedtls_ssl_init(&_ctrl_ssl);
    mbedtls_net_init(&_ctrl_net);
    mbedtls_ssl_session_init(&_ctrl_session);
}

PrinterFtp::~PrinterFtp() {
    quit();
    mbedtls_ssl_session_free(&_ctrl_session);
    mbedtls_ssl_free(&_ctrl_ssl);
    mbedtls_net_free(&_ctrl_net);
    if (_mbed_inited) {
        mbedtls_ssl_config_free(&_conf);
        mbedtls_ctr_drbg_free(&_drbg);
        mbedtls_entropy_free(&_entropy);
    }
}

void PrinterFtp::_emit(const char* step, int code, const String& text) {
    if (!_trace) return;
    uint32_t t = _traceStart ? (millis() - _traceStart) : 0;
    _trace(step, code, text, t);
}

bool PrinterFtp::_initMbedtlsOnce() {
    if (_mbed_inited) return true;
    _install_psram_alloc_once();   // before any mbedtls_*_init
    mbedtls_entropy_init(&_entropy);
    mbedtls_ctr_drbg_init(&_drbg);
    mbedtls_ssl_config_init(&_conf);

    int rc = mbedtls_ctr_drbg_seed(&_drbg, mbedtls_entropy_func, &_entropy,
                                   (const unsigned char*)"spoolhard-ftps", 14);
    if (rc != 0) { _lastError = "ctr_drbg_seed " + _mbedErr(rc); return false; }
    rc = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { _lastError = "ssl_config_defaults " + _mbedErr(rc); return false; }
    // Bambu's printer cert is self-signed (CN=<serial>). We only ever
    // talk to the printer we were configured to talk to, so cert
    // verification adds no real protection — turn it off so the
    // handshake doesn't reject the cert chain. Same posture as
    // WiFiClientSecure::setInsecure() and the lftp test.
    mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_drbg);
    // Cap TLS fragment length so each ssl_context allocates ~8 KiB of
    // I/O buffers instead of the default ~32 KiB. With control AND data
    // contexts live during a transfer (RFC 4217 session reuse demands
    // it), the internal-DRAM budget on ESP32-S3 doesn't fit two
    // 32 KiB-buffered contexts — ssl_setup() returns SSL_ALLOC_FAILED.
    // 4096-byte records work fine for FTPS payloads.
    mbedtls_ssl_conf_max_frag_len(&_conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    _mbed_inited = true;
    return true;
}

void PrinterFtp::_resetCtrl() {
    if (_ctrl_open) {
        mbedtls_ssl_close_notify(&_ctrl_ssl);
        _ctrl_open = false;
    }
    mbedtls_ssl_free(&_ctrl_ssl);
    mbedtls_net_free(&_ctrl_net);
    mbedtls_ssl_init(&_ctrl_ssl);
    mbedtls_net_init(&_ctrl_net);
    if (_ctrl_session_saved) {
        mbedtls_ssl_session_free(&_ctrl_session);
        mbedtls_ssl_session_init(&_ctrl_session);
        _ctrl_session_saved = false;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Control I/O
// ─────────────────────────────────────────────────────────────────────

int PrinterFtp::_readResponse(String* body, uint32_t timeout_ms) {
    // FTP response: <3-digit><space-or-dash><text>\r\n. Multi-line
    // responses use '-' on the first line and '<same code> ' on the
    // terminating line. We read from the TLS stream until that
    // terminator appears, accumulating partial lines across reads.
    uint32_t start = millis();
    String line;
    int code = -1;
    bool multi = false;
    String multi_code;

    while (millis() - start < timeout_ms) {
        unsigned char tbuf[256];
        int n = mbedtls_ssl_read(&_ctrl_ssl, tbuf, sizeof(tbuf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            _lastError = "ctrl read " + (n == 0 ? String("eof") : _mbedErr(n));
            return -1;
        }
        for (int i = 0; i < n; ++i) {
            char c = (char)tbuf[i];
            if (c == '\n') {
                // Strip trailing CR.
                if (line.length() && line[line.length() - 1] == '\r') line.remove(line.length() - 1);
                if (body) { if (body->length()) *body += '\n'; *body += line; }
                if (line.length() >= 4 && isdigit((unsigned char)line[0]) &&
                    isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2])) {
                    int thisCode = atoi(line.substring(0, 3).c_str());
                    if (line[3] == '-' && !multi) {
                        multi = true;
                        multi_code = line.substring(0, 3);
                        code = thisCode;
                    } else if (line[3] == ' ') {
                        if (!multi || line.substring(0, 3) == multi_code) {
                            return thisCode;
                        }
                    }
                }
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
    }
    _lastError = "response timeout";
    return -1;
}

int PrinterFtp::_sendCmd(const String& cmd) {
    String line = cmd + "\r\n";
    Serial.printf("[FTP] > %s\n", cmd.c_str());
    int rc = mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)line.c_str(), line.length());
    if (rc < 0) {
        _lastError = "ctrl write " + _mbedErr(rc);
        return -1;
    }
    String body;
    int code = _readResponse(&body);
    String label = cmd.startsWith("PASS") ? String("PASS ********") : cmd;
    _emit(label.c_str(), code, body);
    return code;
}

// ─────────────────────────────────────────────────────────────────────
// connect / quit
// ─────────────────────────────────────────────────────────────────────

bool PrinterFtp::connect(const IPAddress& ip, const String& access_code,
                         const String& sni_hostname, uint16_t port) {
    _ip  = ip;
    _sni = sni_hostname;
    _lastError = "";

    if (!_initMbedtlsOnce()) return false;
    _resetCtrl();

    _emit("connect", 0,
          String("tls://") + ip.toString() + ":" + String(port) + " sni=" + sni_hostname);
    Serial.printf("[FTP] Connecting to %s:%u (implicit TLS, SNI=%s)\n",
                  ip.toString().c_str(), port, sni_hostname.c_str());

    char port_s[8]; snprintf(port_s, sizeof(port_s), "%u", port);
    int rc = mbedtls_net_connect(&_ctrl_net, ip.toString().c_str(), port_s,
                                 MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        _lastError = "ctrl tcp " + _mbedErr(rc);
        _emit("ctrl tcp", -1, _lastError);
        return false;
    }
    rc = mbedtls_ssl_setup(&_ctrl_ssl, &_conf);
    if (rc != 0) {
        _lastError = "ctrl ssl_setup " + _mbedErr(rc);
        _emit("ctrl ssl_setup", -1, _lastError);
        return false;
    }
    if (sni_hostname.length()) {
        rc = mbedtls_ssl_set_hostname(&_ctrl_ssl, sni_hostname.c_str());
        if (rc != 0) {
            _lastError = "ctrl set_hostname " + _mbedErr(rc);
            return false;
        }
    }
    mbedtls_ssl_set_bio(&_ctrl_ssl, &_ctrl_net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&_ctrl_ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            _lastError = "ctrl handshake " + _mbedErr(rc);
            _emit("tls handshake", -1, _lastError);
            return false;
        }
    }
    _ctrl_open = true;
    _emit("tls handshake", 0, "ok");

    // Banner.
    String bannerBody;
    int banner = _readResponse(&bannerBody);
    _emit("banner", banner, bannerBody);
    if (banner != 220) {
        _lastError = "no 220 banner (got " + String(banner) + ")";
        return false;
    }

    if (_sendCmd("USER bblp") != 331) { _lastError = "USER rejected"; return false; }
    if (_sendCmd("PASS " + access_code) != 230) { _lastError = "PASS rejected (wrong access code?)"; return false; }
    if (_sendCmd("TYPE I") != 200) { _lastError = "TYPE I rejected"; return false; }
    // PBSZ + PROT must be sent BEFORE the data session can be opened
    // and before we save the session for reuse — Bambu enforces both.
    if (_sendCmd("PBSZ 0") != 200) { _lastError = "PBSZ 0 rejected"; return false; }
    if (_sendCmd("PROT P") != 200) { _lastError = "PROT P rejected"; return false; }

    // Capture the post-login control session so every subsequent data
    // channel can resume it. RFC 4217 §10.1: Bambu's FTPS daemon (and
    // many others) will reject the data handshake otherwise with a
    // TLS fatal alert.
    rc = mbedtls_ssl_get_session(&_ctrl_ssl, &_ctrl_session);
    if (rc != 0) {
        _lastError = "ssl_get_session " + _mbedErr(rc);
        return false;
    }
    _ctrl_session_saved = true;

    Serial.println("[FTP] Authenticated; control session saved for data reuse");
    return true;
}

bool PrinterFtp::quit() {
    if (_ctrl_open) {
        // Best-effort QUIT — ignore errors.
        const char* q = "QUIT\r\n";
        mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)q, 6);
        String body; _readResponse(&body, 1000);
        mbedtls_ssl_close_notify(&_ctrl_ssl);
        _ctrl_open = false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// PASV / data channel
// ─────────────────────────────────────────────────────────────────────

bool PrinterFtp::_parsePasv(const String& line, IPAddress& ip, uint16_t& port) {
    int open  = line.indexOf('(');
    int close = line.indexOf(')');
    if (open < 0 || close <= open) return false;
    String nums = line.substring(open + 1, close);
    int values[6] = {0};
    int idx = 0, start = 0;
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

bool PrinterFtp::_openDataChannel(mbedtls_net_context* data_net,
                                  mbedtls_ssl_context* data_ssl) {
    if (!_ctrl_open) { _lastError = "control closed"; return false; }
    if (!_ctrl_session_saved) {
        _lastError = "no saved control session — connect() not run?";
        return false;
    }

    // PASV — Bambu's data port is one-shot per PASV.
    int code = mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)"PASV\r\n", 6);
    if (code < 0) { _lastError = "PASV write " + _mbedErr(code); return false; }
    String pasvBody;
    code = _readResponse(&pasvBody);
    _emit("PASV", code, pasvBody);
    if (code != 227) { _lastError = "PASV failed (got " + String(code) + ")"; return false; }
    IPAddress dip; uint16_t dport;
    if (!_parsePasv(pasvBody, dip, dport)) { _lastError = "PASV parse failed"; return false; }
    if ((uint32_t)dip == 0) dip = _ip;   // 0.0.0.0 → control IP

    mbedtls_net_init(data_net);
    mbedtls_ssl_init(data_ssl);

    char dport_s[8]; snprintf(dport_s, sizeof(dport_s), "%u", dport);
    int rc = mbedtls_net_connect(data_net, dip.toString().c_str(), dport_s,
                                 MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        _lastError = "data tcp " + _mbedErr(rc);
        _emit("data tcp", -1, _lastError + " " + dip.toString() + ":" + String(dport));
        mbedtls_ssl_free(data_ssl);
        mbedtls_net_free(data_net);
        return false;
    }
    rc = mbedtls_ssl_setup(data_ssl, &_conf);
    if (rc != 0) {
        _lastError = "data ssl_setup " + _mbedErr(rc);
        mbedtls_ssl_free(data_ssl);
        mbedtls_net_free(data_net);
        return false;
    }
    if (_sni.length()) {
        rc = mbedtls_ssl_set_hostname(data_ssl, _sni.c_str());
        if (rc != 0) {
            _lastError = "data set_hostname " + _mbedErr(rc);
            mbedtls_ssl_free(data_ssl);
            mbedtls_net_free(data_net);
            return false;
        }
    }
    // The whole point — resume the control session so Bambu's FTPS
    // daemon accepts the data handshake.
    rc = mbedtls_ssl_set_session(data_ssl, &_ctrl_session);
    if (rc != 0) {
        _lastError = "data set_session " + _mbedErr(rc);
        mbedtls_ssl_free(data_ssl);
        mbedtls_net_free(data_net);
        return false;
    }
    mbedtls_ssl_set_bio(data_ssl, data_net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(data_ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            _lastError = "data handshake " + _mbedErr(rc) + " (session reuse rejected?)";
            _emit("data handshake", -1, _lastError);
            mbedtls_ssl_close_notify(data_ssl);
            mbedtls_ssl_free(data_ssl);
            mbedtls_net_free(data_net);
            return false;
        }
    }
    _emit("data handshake", 0,
          String("ok ") + dip.toString() + ":" + String(dport));
    return true;
}

void PrinterFtp::_closeData(mbedtls_net_context* data_net,
                            mbedtls_ssl_context* data_ssl) {
    mbedtls_ssl_close_notify(data_ssl);
    mbedtls_ssl_free(data_ssl);
    mbedtls_net_free(data_net);
}

// ─────────────────────────────────────────────────────────────────────
// RETR-style data ops (range, stream, into, trailing) + LIST + SIZE
// ─────────────────────────────────────────────────────────────────────

bool PrinterFtp::_openDataAndRetrieve(const String& path, uint32_t offset,
                                      std::function<bool(mbedtls_ssl_context&)> reader) {
    mbedtls_net_context data_net;
    mbedtls_ssl_context data_ssl;
    if (!_openDataChannel(&data_net, &data_ssl)) return false;

    // REST <offset> for range requests. Sent on control AFTER data
    // channel is up so PASV's port stays valid.
    bool did_rest = false;
    if (offset > 0) {
        char buf[32]; snprintf(buf, sizeof(buf), "REST %u", offset);
        if (_sendCmd(buf) == 350) did_rest = true;
    }

    // RETR — server replies 150 (or 125) on control.
    String retr = "RETR " + path + "\r\n";
    int rc = mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)retr.c_str(), retr.length());
    if (rc < 0) {
        _lastError = "RETR write " + _mbedErr(rc);
        _closeData(&data_net, &data_ssl);
        return false;
    }
    int retr_code = _readResponse();
    if (retr_code != 150 && retr_code != 125) {
        _lastError = "RETR rejected (got " + String(retr_code) + ")";
        _closeData(&data_net, &data_ssl);
        return false;
    }

    // If REST didn't take, burn `offset` bytes off the data stream.
    if (!did_rest && offset > 0) {
        uint32_t remaining = offset;
        unsigned char scratch[512];
        uint32_t start = millis();
        while (remaining > 0 && millis() - start < 30000) {
            int n = mbedtls_ssl_read(&data_ssl, scratch,
                                     remaining > sizeof(scratch) ? sizeof(scratch) : remaining);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            if (n <= 0) break;
            remaining -= n;
            start = millis();
        }
    }

    bool ok = reader(data_ssl);
    _closeData(&data_net, &data_ssl);

    // Drain the 226 transfer-complete on control.
    _readResponse();
    return ok;
}

bool PrinterFtp::retrieveRange(const String& path, uint32_t offset, uint32_t length, RangeCb cb) {
    uint32_t read_total = 0;
    return _openDataAndRetrieve(path, offset, [&](mbedtls_ssl_context& d) {
        unsigned char buf[1024];
        uint32_t start = millis();
        while (read_total < length && millis() - start < 30000) {
            uint32_t want = length - read_total;
            if (want > sizeof(buf)) want = sizeof(buf);
            int n = mbedtls_ssl_read(&d, buf, want);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (n <= 0) break;
            if (!cb(buf, (size_t)n)) return false;
            read_total += n;
            start = millis();
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
    return _openDataAndRetrieve(path, 0, [&](mbedtls_ssl_context& d) {
        unsigned char buf[1024];
        uint32_t start = millis();
        while (millis() - start < 60000) {
            int n = mbedtls_ssl_read(&d, buf, sizeof(buf));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (n <= 0) break;
            total += n;
            if (!cb(buf, (size_t)n, total)) return false;
            start = millis();
        }
        return true;
    });
}

bool PrinterFtp::retrieveTrailing(const String& path, uint32_t tail_size,
                                  uint8_t* dst, uint32_t* streamed_total,
                                  uint32_t* tail_actual) {
    if (!dst || tail_size == 0) { _lastError = "bad args"; return false; }
    uint32_t total = 0;
    uint32_t pos = 0;
    bool ok = retrieveStream(path, [&](const uint8_t* data, size_t n, size_t) {
        total += n;
        size_t take = n;
        const uint8_t* src = data;
        if (take > tail_size) {
            src += (take - tail_size);
            take = tail_size;
        }
        while (take > 0) {
            size_t to_end = (size_t)tail_size - pos;
            size_t w = (take < to_end) ? take : to_end;
            memcpy(dst + pos, src, w);
            pos = (pos + w) % tail_size;
            src += w;
            take -= w;
        }
        return true;
    });
    if (!ok) return false;
    if (streamed_total) *streamed_total = total;
    if (tail_actual)    *tail_actual    = (total < tail_size) ? total : tail_size;
    if (total >= tail_size) std::rotate(dst, dst + pos, dst + tail_size);
    return true;
}

int32_t PrinterFtp::size(const String& path) {
    String cmd = "SIZE " + path + "\r\n";
    int rc = mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)cmd.c_str(), cmd.length());
    if (rc < 0) { _lastError = "SIZE write " + _mbedErr(rc); return -1; }
    String body;
    int code = _readResponse(&body);
    if (code != 213) return -1;
    int sp = body.indexOf(' ');
    if (sp < 0) return -1;
    return body.substring(sp + 1).toInt();
}

bool PrinterFtp::listDir(const String& path, std::vector<String>& out) {
    out.clear();
    mbedtls_net_context data_net;
    mbedtls_ssl_context data_ssl;
    if (!_openDataChannel(&data_net, &data_ssl)) return false;

    String cmd = "LIST " + path + "\r\n";
    int rc = mbedtls_ssl_write(&_ctrl_ssl, (const unsigned char*)cmd.c_str(), cmd.length());
    if (rc < 0) {
        _lastError = "LIST write " + _mbedErr(rc);
        _closeData(&data_net, &data_ssl);
        return false;
    }
    String body;
    int list_code = _readResponse(&body);
    _emit(("LIST " + path).c_str(), list_code, body);
    if (list_code != 150 && list_code != 125) {
        _lastError = "LIST rejected (got " + String(list_code) + ")";
        _closeData(&data_net, &data_ssl);
        return false;
    }

    // Drain the data channel — server closes it (PEER_CLOSE_NOTIFY)
    // when the listing is done.
    String line;
    uint32_t start = millis();
    while (millis() - start < 15000) {
        unsigned char tbuf[512];
        int n = mbedtls_ssl_read(&data_ssl, tbuf, sizeof(tbuf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            char c = (char)tbuf[i];
            if (c == '\n') {
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
    _closeData(&data_net, &data_ssl);

    // 226 on control to confirm transfer complete.
    String endBody;
    int end_code = _readResponse(&endBody);
    _emit("LIST complete", end_code, endBody);
    _emit("listing", 0, String(out.size()) + " entr" + (out.size() == 1 ? "y" : "ies"));
    return true;
}
