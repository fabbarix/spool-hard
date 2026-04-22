#include "web_server.h"
#include "config.h"
#include "ota.h"
#include "load_cell.h"
#include "console_channel.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <Update.h>

// ── Lifecycle ────────────────────────────────────────────────

void ScaleWebServer::begin() {
    _setupRoutes();
}

void ScaleWebServer::start() {
    _server.begin();
    Serial.println("[WebServer] Started on port 80");
}

// ── Debug broadcast ──────────────────────────────────────────

void ScaleWebServer::broadcastDebug(const String& type, const JsonDocument& payload) {
    if (_ws.count() == 0) return;
    JsonDocument env;
    env["type"] = type;
    env["data"] = payload;
    String out;
    serializeJson(env, out);
    _ws.textAll(out);
}

void ScaleWebServer::broadcastConsoleFrame(const char* dir, const String& frame) {
    if (_ws.count() == 0) return;
    JsonDocument env;
    env["type"]  = "console";
    env["dir"]   = dir;
    env["frame"] = frame;
    String out;
    serializeJson(env, out);
    _ws.textAll(out);
}

// ── Route setup ──────────────────────────────────────────────


// ── Auth ─────────────────────────────────────────────────────

bool ScaleWebServer::_requireAuth(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String stored = prefs.getString(NVS_KEY_FIXED_KEY, "");
    prefs.end();

    if (stored.isEmpty() || stored == DEFAULT_FIXED_KEY) return true;

    String auth = req->header("Authorization");
    if (auth.startsWith("Bearer ") && auth.substring(7) == stored) return true;

    if (req->hasParam("key") && req->getParam("key")->value() == stored) return true;

    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
}

void ScaleWebServer::_handleAuthStatus(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String stored = prefs.getString(NVS_KEY_FIXED_KEY, "");
    String device = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardScale");
    prefs.end();

    bool required = !stored.isEmpty() && stored != DEFAULT_FIXED_KEY;
    bool authed   = !required;
    if (required) {
        String auth = req->header("Authorization");
        if (auth.startsWith("Bearer ") && auth.substring(7) == stored) authed = true;
    }
    JsonDocument doc;
    doc["auth_required"] = required;
    doc["authenticated"] = authed;
    doc["device_name"]   = device;
    doc["product"]       = "scale";
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ScaleWebServer::_setupRoutes() {
    // Debug WebSocket for live dashboard events. On connect, push a
    // snapshot of the current console-link state so the dashboard doesn't
    // sit at "Console N/A" if the console paired before the UI was opened
    // (the console_conn broadcast otherwise only fires on edge events).
    _ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                   void*, uint8_t*, size_t) {
        if (type != WS_EVT_CONNECT) return;
        JsonDocument env;
        env["type"] = "console_conn";
        JsonObject data = env["data"].to<JsonObject>();
        data["connected"] = g_console.isConnected();
        if (g_console.isConnected()) data["ip"] = g_console.lastClientIp();
        String out; serializeJson(env, out);
        client->text(out);
    });
    _server.addHandler(&_ws);

    // Always-open auth probe. Never 401s; reports whether a key is set
    // and whether the supplied Authorization header is valid.
    _server.on("/api/auth-status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleAuthStatus(req);
    });


    // ── API endpoints (registered before static handler for priority) ──

    _server.on("/api/nfc-module-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleNfcConfig(req);
    });
    _server.on("/api/nfc-module-config", HTTP_POST, [this](AsyncWebServerRequest* req) {
        _handleNfcConfig(req);
    });

    _server.on("/api/device-name-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleDeviceName(req);
    });
    _server.on("/api/device-name-config", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            String name = doc["device_name"] | "";
            if (name.isEmpty()) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"name required\"}");
                return;
            }
            Preferences prefs;
            prefs.begin(NVS_NS_WIFI, false);
            prefs.putString(NVS_KEY_DEVICE_NAME, name);
            prefs.end();
            Serial.printf("[Config] Device name changed to '%s'\n", name.c_str());
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    _server.on("/api/wifi-status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleWifiStatus(req);
    });

    _server.on("/api/ota-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleOtaConfigGet(req);
    });
    _server.on("/api/ota-config", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleOtaConfigPost(req, data, len);
        }
    );

    _server.on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    _server.on("/api/reset-device", HTTP_POST, [this](AsyncWebServerRequest* req) {
        _handleReset(req);
    });

    _server.on("/api/test-key", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleTestKey(req);
    });
    _server.on("/api/fixed-key-config", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleFixedKeyConfigPost(req, data, len);
        }
    );

    _server.on("/api/tags-in-store", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (!SPIFFS.exists("/tags_in_store.txt")) {
            req->send(200, "application/json", "{\"tags\":\"\"}");
            return;
        }
        File f = SPIFFS.open("/tags_in_store.txt", FILE_READ);
        String tags = f.readString();
        f.close();
        JsonDocument doc;
        doc["tags"] = tags;
        String resp;
        serializeJson(doc, resp);
        req->send(200, "application/json", resp);
    });

    // ── Scale config + actions ─────────────────────────────

    _server.on("/api/scale-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleScaleConfigGet(req);
    });
    _server.on("/api/scale-config", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleScaleConfigPost(req, data, len);
        }
    );
    _server.on("/api/scale-tare", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (_onTare) _onTare();
        // Return the fresh tare so the client can patch its cache without a
        // follow-up GET — the calibration page shows tare_raw directly, and
        // invalidate-and-refetch adds a visible flicker while the new blob
        // is in flight. _onCaptureRaw would re-sample (slow, ~500 ms for
        // 20 averages); instead pull the just-written tare_raw out of NVS.
        long newTare = 0;
        {
            Preferences p;
            p.begin(NVS_NS_CALIBRATION, true);
            newTare = p.getLong(NVS_KEY_ZERO, 0);
            p.end();
        }
        char body[64];
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"tare_raw\":%ld}", newTare);
        req->send(200, "application/json", body);
    });
    _server.on("/api/scale-calibrate", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            float weight = doc["weight"] | 0.0f;
            if (weight <= 0) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"weight must be > 0\"}");
                return;
            }
            if (_onCalibrate) _onCalibrate(weight);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // Capture current raw ADC reading (averaged)
    _server.on("/api/scale-raw", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        long raw = _onCaptureRaw ? _onCaptureRaw() : 0;
        JsonDocument doc;
        doc["raw"] = raw;
        String resp;
        serializeJson(doc, resp);
        req->send(200, "application/json", resp);
    });

    // Add a calibration point (captures raw at the moment of call)
    _server.on("/api/scale-cal-point", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            float weight = doc["weight"] | 0.0f;
            if (weight <= 0) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"weight must be > 0\"}");
                return;
            }
            if (_onAddCalPoint) _onAddCalPoint(weight);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // Clear all calibration points
    _server.on("/api/scale-cal-clear", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (_onClearCal) _onClearCal();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ── Firmware info ────────────────────────────────────────

    _server.on("/api/firmware-info", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleFirmwareInfo(req);
    });

    // ── Direct binary upload: firmware ────────────────────────

    _server.on("/api/upload/firmware", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!_requireAuth(req)) return;
            if (_uploadRejectedProduct) {
                req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"wrong product — expected " PRODUCT_NAME " firmware\"}");
                _uploadRejectedProduct = false;
                return;
            }
            bool ok = !Update.hasError();
            req->send(ok ? 200 : 500, "application/json",
                      ok ? "{\"ok\":true}" : "{\"ok\":false}");
            if (ok) { delay(1000); ESP.restart(); }
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (index == 0 && !_requireAuth(req)) { Update.abort(); return; }
            if (index == 0) {
                Serial.printf("[Upload] Firmware: %s\n", filename.c_str());
                if (_onUploadStarted) _onUploadStarted("firmware");
                _uploadMatcher.reset();
                _uploadRejectedProduct = false;
                _uploadIsSpiffs        = false;
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Serial.printf("[Upload] Begin failed: %s\n", Update.errorString());
                    return;
                }
            }
            _uploadMatcher.feed(data, len);
            if (Update.isRunning()) Update.write(data, len);
            if (_onUploadProgress) _onUploadProgress();   // LED liveness ping
            if (final) {
                if (!_uploadMatcher.matched()) {
                    // Whole file arrived but never contained our product tag.
                    // Abort the OTA slot so the running firmware stays live.
                    Serial.println("[Upload] Firmware rejected: product signature missing");
                    Update.abort();
                    _uploadRejectedProduct = true;
                    return;
                }
                if (Update.end(true)) {
                    Serial.printf("[Upload] Firmware OK: %u bytes\n", index + len);
                } else {
                    Serial.printf("[Upload] Firmware failed: %s\n", Update.errorString());
                }
            }
        }
    );

    // ── Direct binary upload: frontend (served from SPIFFS) ──
    // The API path stays /api/upload/spiffs so existing clients keep working;
    // only the release-bundle filename and user-visible labels are renamed.

    _server.on("/api/upload/spiffs", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!_requireAuth(req)) return;
            if (_uploadRejectedProduct) {
                req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"wrong product — expected " PRODUCT_NAME " frontend\"}");
                _uploadRejectedProduct = false;
                return;
            }
            bool ok = !Update.hasError();
            req->send(ok ? 200 : 500, "application/json",
                      ok ? "{\"ok\":true}" : "{\"ok\":false}");
            if (ok) { delay(1000); ESP.restart(); }
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (index == 0 && !_requireAuth(req)) { Update.abort(); return; }
            if (index == 0) {
                Serial.printf("[Upload] Frontend: %s\n", filename.c_str());
                if (_onUploadStarted) _onUploadStarted("spiffs");
                _uploadMatcher.reset();
                _uploadRejectedProduct = false;
                _uploadIsSpiffs        = true;
                if (!Update.begin(0x1f0000, U_SPIFFS)) {
                    Serial.printf("[Upload] Begin failed: %s\n", Update.errorString());
                    return;
                }
            }
            _uploadMatcher.feed(data, len);
            if (Update.isRunning()) Update.write(data, len);
            if (_onUploadProgress) _onUploadProgress();   // LED liveness ping
            if (final) {
                if (!_uploadMatcher.matched()) {
                    // Unlike firmware, SPIFFS writes have already landed in
                    // the partition by now. Abort + format so the device
                    // recovers to a clean empty FS on next boot instead of
                    // trying to mount a half-written image of the wrong
                    // product.
                    Serial.println("[Upload] Frontend rejected: product signature missing");
                    Update.abort();
                    SPIFFS.format();
                    _uploadRejectedProduct = true;
                    return;
                }
                if (Update.end(true)) {
                    Serial.printf("[Upload] Frontend OK: %u bytes\n", index + len);
                } else {
                    Serial.printf("[Upload] Frontend failed: %s\n", Update.errorString());
                }
            }
        }
    );

    // ── Static SPA from SPIFFS (must be after API routes) ────

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (SPIFFS.exists("/index.html.gz")) {
            auto* resp = req->beginResponse(SPIFFS, "/index.html.gz", "text/html");
            resp->addHeader("Content-Encoding", "gzip");
            req->send(resp);
        } else {
            req->send(SPIFFS, "/index.html", "text/html");
        }
    });

    _server.serveStatic("/", SPIFFS, "/")
           .setCacheControl("max-age=86400");

    // SPA fallback: non-API GET requests serve index.html for client-side routing
    // (history API paths like /configuration, /dashboard)
    _server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_GET &&
            !req->url().startsWith("/api/") &&
            !req->url().startsWith("/captive/")) {
            if (SPIFFS.exists("/index.html.gz")) {
                auto* resp = req->beginResponse(SPIFFS, "/index.html.gz", "text/html");
                resp->addHeader("Content-Encoding", "gzip");
                req->send(resp);
            } else if (SPIFFS.exists("/index.html")) {
                req->send(SPIFFS, "/index.html", "text/html");
            } else {
                req->send(200, "text/html", "<h1>SpoolHardScale</h1><p>Frontend not installed. Upload via /api/upload/spiffs.</p>");
            }
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });
}

// ── Handlers ─────────────────────────────────────────────────

void ScaleWebServer::_handleNfcConfig(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_NFC, true);
    bool available = prefs.getBool(NVS_KEY_NFC_AVAIL, false);
    prefs.end();

    JsonDocument doc;
    doc["available"] = available;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleDeviceName(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String name = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardScale");
    prefs.end();

    JsonDocument doc;
    doc["device_name"] = name;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleWifiStatus(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    bool   configured = prefs.isKey(NVS_KEY_SSID);
    String ssid       = prefs.getString(NVS_KEY_SSID, "");
    prefs.end();

    bool connected = (WiFi.status() == WL_CONNECTED);

    JsonDocument doc;
    doc["configured"] = configured && !ssid.isEmpty();
    doc["connected"]  = connected;
    doc["ssid"]       = connected ? WiFi.SSID() : ssid;
    doc["ip"]         = connected ? WiFi.localIP().toString() : "";
    doc["rssi"]       = connected ? WiFi.RSSI() : 0;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleOtaConfigGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    OtaConfig cfg;
    cfg.load();

    JsonDocument doc;
    doc["url"]        = cfg.url;
    doc["use_ssl"]    = cfg.use_ssl;
    doc["verify_ssl"] = cfg.verify_ssl;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleOtaConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }

    OtaConfig cfg;
    cfg.load();

    if (doc["url"].is<const char*>())    cfg.url        = doc["url"].as<String>();
    if (doc["use_ssl"].is<bool>())       cfg.use_ssl    = doc["use_ssl"];
    if (doc["verify_ssl"].is<bool>())    cfg.verify_ssl = doc["verify_ssl"];
    if (!cfg.use_ssl) cfg.verify_ssl = false;

    cfg.save();

    JsonDocument resp_doc;
    resp_doc["url"]        = cfg.url;
    resp_doc["use_ssl"]    = cfg.use_ssl;
    resp_doc["verify_ssl"] = cfg.verify_ssl;
    String resp;
    serializeJson(resp_doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleReset(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    req->send(200, "application/json", "{\"status\":\"resetting\"}");
    delay(500);
    ESP.restart();
}

void ScaleWebServer::_handleTestKey(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    bool   configured = prefs.isKey(NVS_KEY_FIXED_KEY);
    String key        = prefs.getString(NVS_KEY_FIXED_KEY, DEFAULT_FIXED_KEY);
    prefs.end();

    String masked = key;
    if (key.length() > 4)
        masked = key.substring(0, 2) + "***" + key.substring(key.length() - 2);

    JsonDocument doc;
    doc["configured"]  = configured;
    doc["key_preview"] = masked;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleFixedKeyConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }
    String key = doc["key"] | "";
    if (key.isEmpty()) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"key required\"}");
        return;
    }

    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, false);
    prefs.putString(NVS_KEY_FIXED_KEY, key);
    prefs.end();

    Serial.printf("[Security] Fixed key updated\n");
    req->send(200, "application/json", "{\"ok\":true}");
}

void ScaleWebServer::_handleScaleConfigGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, true);
    JsonDocument doc;
    doc["samples"]          = prefs.getInt(NVS_KEY_SAMPLES,     WEIGHT_SAMPLES);
    doc["stable_threshold"] = prefs.getFloat(NVS_KEY_STABLE_THR, STABLE_THRESHOLD_G);
    doc["stable_count"]     = prefs.getInt(NVS_KEY_STABLE_CNT,  STABLE_COUNT_REQ);
    doc["load_detect"]      = prefs.getFloat(NVS_KEY_LOAD_DET,   LOAD_DETECT_G);
    doc["precision"]        = prefs.getInt(NVS_KEY_PRECISION,   1);
    doc["rounding"]         = prefs.getUChar(NVS_KEY_ROUNDING,  0) == 0 ? "round" : "truncate";
    prefs.end();

    // Calibration data
    Preferences calPrefs;
    calPrefs.begin(NVS_NS_CALIBRATION, true);
    long tare = calPrefs.getLong(NVS_KEY_ZERO, 0);
    int numPts = calPrefs.getInt("num_pts", 0);
    doc["tare_raw"] = tare;
    doc["num_points"] = numPts;

    // cal_pts as written by the firmware (schema v2): CalPoint.delta is the
    // ADC-count offset from tare at capture time. We surface that as
    // `delta` in the JSON so the UI can show a tare-invariant number.
    // On a fresh device running this firmware there's no v1 blob to worry
    // about — LoadCell::loadCalibration() migrates at boot and rewrites
    // the bytes in v2 shape before this endpoint gets a chance to serve.
    int schemaV = calPrefs.getInt("cal_schema_v", 2);
    CalPoint pts[MAX_CAL_POINTS];
    if (numPts > 0 && numPts <= MAX_CAL_POINTS) {
        calPrefs.getBytes("cal_pts", pts, sizeof(CalPoint) * numPts);
        JsonArray arr = doc["points"].to<JsonArray>();
        for (int i = 0; i < numPts; i++) {
            JsonObject p = arr.add<JsonObject>();
            // Defensive: if for some reason the blob is still v1 when we
            // read it here, convert on the fly so the UI sees consistent
            // delta values. (Should never fire — LoadCell::begin() runs
            // before the web server starts.)
            long delta = (schemaV < 2) ? (pts[i].delta - tare) : pts[i].delta;
            p["delta"]    = delta;
            p["weight_g"] = pts[i].weight_g;
        }
    }
    doc["calibrated"] = (numPts > 0 && tare != 0);

    // Legacy single-point fallback — only triggers if the multipoint blob
    // is empty but old single-point keys are still populated. Same delta
    // semantics as the multipoint path above.
    if (numPts == 0) {
        float legacyWeight = calPrefs.getFloat(NVS_KEY_CALIB_WEIGHT, 0.0f);
        long legacyRaw = calPrefs.getLong(NVS_KEY_CALIB_LC, 0);
        if (legacyWeight > 0 && legacyRaw != 0 && legacyRaw != tare) {
            doc["calibrated"] = true;
            doc["num_points"] = 1;
            JsonArray arr = doc["points"].to<JsonArray>();
            JsonObject p = arr.add<JsonObject>();
            p["delta"]    = legacyRaw - tare;
            p["weight_g"] = legacyWeight;
        }
    }
    calPrefs.end();

    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void ScaleWebServer::_handleScaleConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }

    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, false);
    if (doc["samples"].is<int>())            prefs.putInt(NVS_KEY_SAMPLES,     doc["samples"].as<int>());
    if (doc["stable_threshold"].is<float>()) prefs.putFloat(NVS_KEY_STABLE_THR, doc["stable_threshold"].as<float>());
    if (doc["stable_count"].is<int>())       prefs.putInt(NVS_KEY_STABLE_CNT,  doc["stable_count"].as<int>());
    if (doc["load_detect"].is<float>())      prefs.putFloat(NVS_KEY_LOAD_DET,   doc["load_detect"].as<float>());
    if (doc["precision"].is<int>())          prefs.putInt(NVS_KEY_PRECISION,   constrain(doc["precision"].as<int>(), 0, 4));
    if (doc["rounding"].is<const char*>()) {
        const char* r = doc["rounding"];
        prefs.putUChar(NVS_KEY_ROUNDING, strcmp(r, "truncate") == 0 ? 1 : 0);
    }
    prefs.end();

    // Apply the fresh values to the live LoadCell immediately — main.cpp's
    // handler calls g_scale.loadParams() to re-read NVS and update the
    // in-memory _params. Without this, the user would have to reboot the
    // device before precision / rounding / sampling changes took effect.
    if (_onConfigChanged) _onConfigChanged();

    Serial.println("[Config] Scale params updated");
    req->send(200, "application/json", "{\"ok\":true}");
}

void ScaleWebServer::_handleFirmwareInfo(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    doc["fw_version"]   = FW_VERSION;
    doc["fe_version"]   = FE_VERSION;
    doc["flash_size"]   = ESP.getFlashChipSize();
    doc["spiffs_total"] = SPIFFS.totalBytes();
    doc["spiffs_used"]  = SPIFFS.usedBytes();
    doc["free_heap"]    = ESP.getFreeHeap();
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}
