#include "web_server.h"
#include "config.h"
#include "display.h"
#include "spoolhard/ota.h"
#include "spoolhard/backup.h"
#include "store.h"
#include "user_filaments_store.h"
#include "stock_filaments_store.h"
#include "bambu_cloud_filaments.h"
#include "ring_log.h"
#include "scale_link.h"
#include "scale_secrets.h"
#include "core_weights.h"
#include "quick_weights.h"
#include "sdcard.h"
#include "printer_config.h"
#include "bambu_manager.h"
#include "bambu_cloud.h"
#include "bambu_discovery.h"
#include "scale_discovery.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <LittleFS.h>
#include <SD.h>
#include <Update.h>

// ── Lifecycle ────────────────────────────────────────────────

// Firmware-version marker bytes. Plant them as a real, non-inline file-scope
// symbol with `used` attribute, AND reference them from begin() below — that
// belt-and-braces guarantees both the compile-time emission and survival
// through --gc-sections, which was eating the previous inline-header version.
namespace {
__attribute__((used))
const char kSpoolHardVersionMarker[] = SPOOLHARD_VERSION_MARKER;
}

// Globals owned by main.cpp; declared up-front so route-registration
// lambdas (which compile inside _setupRoutes below) can reach them.
extern UserFilamentsStore  g_user_filaments;
extern StockFilamentsStore g_stock_filaments;

void ConsoleWebServer::begin()  {
    // The %p reference forces the linker to retain kSpoolHardVersionMarker.
    // Without a code-path use, `__attribute__((used))` alone isn't enough —
    // --gc-sections will collect the .rodata.<sym> section if no incoming
    // reference exists.
    Serial.printf("[WebServer] fw=%s  version marker @ %p\n",
                  FW_VERSION, (const void*)kSpoolHardVersionMarker);
    _setupRoutes();
}
void ConsoleWebServer::start()  { _server.begin(); Serial.println("[WebServer] Listening on :80"); }

void ConsoleWebServer::broadcastDebug(const String& type, const JsonDocument& payload) {
    if (_ws.count() == 0) return;
    JsonDocument env;
    env["type"] = type;
    env["data"] = payload;
    String out;
    serializeJson(env, out);
    _ws.textAll(out);
}

// ── Auth ─────────────────────────────────────────────────────

bool ConsoleWebServer::_requireAuth(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String stored = prefs.getString(NVS_KEY_FIXED_KEY, "");
    prefs.end();

    // No key set, or still the ship-default placeholder → auth is off.
    if (stored.isEmpty() || stored == DEFAULT_FIXED_KEY) return true;

    String auth = req->header("Authorization");
    if (auth.startsWith("Bearer ") && auth.substring(7) == stored) return true;

    // Allow ?key= fallback for multipart uploads and WebSocket handshake.
    if (req->hasParam("key") && req->getParam("key")->value() == stored) return true;

    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
}

void ConsoleWebServer::_handleAuthStatus(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String stored = prefs.getString(NVS_KEY_FIXED_KEY, "");
    String device = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardConsole");
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
    doc["product"]       = "console";
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ── Routes ───────────────────────────────────────────────────

void ConsoleWebServer::_setupRoutes() {
    _ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                   void*, uint8_t*, size_t) {});
    _server.addHandler(&_ws);

    // Always-open auth probe. Never 401s; reports whether a key is set and
    // whether the supplied Authorization header is valid. The frontend uses
    // this both as the pre-login gate check and as the password-verification
    // endpoint on submit.
    _server.on("/api/auth-status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleAuthStatus(req);
    });

    // ── Device config ───────────────────────────────────────
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
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    _server.on("/api/wifi-status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleWifiStatus(req);
    });

    _server.on("/api/ota-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleOtaConfigGet(req);
    });
    _server.on("/api/ota-config", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleOtaConfigPost(req, data, len);
        }
    );
    _server.on("/api/ota-run", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (_onOtaRequested) _onOtaRequested();
        req->send(200, "application/json", "{\"ok\":true}");
    });
    // Periodic-check telemetry + manual triggers.
    _server.on("/api/ota-status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleOtaStatus(req);
    });
    _server.on("/api/ota-check", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        g_ota_checker.kickNow();
        // Phase 5: also forward to the paired scale so a single "Check now"
        // refreshes both products. No-op if the link is down — the cached
        // OtaPending will be cleared on disconnect anyway.
        if (_scale) _scale->requestScaleOtaCheck();
        req->send(200, "application/json", "{\"ok\":true}");
    });
    // Phase-5 trigger: tell the paired scale to flash itself NOW using its
    // stored OtaConfig. Returns 503 if the link is down so the React UI can
    // surface a clear "scale offline, can't update" message.
    _server.on("/api/ota-update-scale", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (!_scale || !_scale->isConnected()) {
            req->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"scale not connected\"}");
            return;
        }
        _scale->requestScaleOtaUpdate();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // Remote tail of the in-RAM ring log. Use ?since=<seq> to poll
    // incrementally; ?since=0 (or omit) returns the last batch (up to
    // ~200 lines). Returns plain text — easy to curl from a terminal
    // and easy to parse from React Query.
    _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        uint32_t since = 0;
        if (req->hasParam("since")) {
            since = (uint32_t)req->getParam("since")->value().toInt();
        }
        auto rows = RingLog::snapshot(since, 200);
        JsonDocument doc;
        doc["head"]  = RingLog::headSeq();
        doc["since"] = since;
        JsonArray arr = doc["lines"].to<JsonArray>();
        for (auto& e : rows) {
            JsonObject o = arr.add<JsonObject>();
            o["seq"]    = e.seq;
            o["t_ms"]   = e.millis_at;
            o["text"]   = e.line;
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

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
    _server.on("/api/fixed-key-config", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleFixedKeyConfigPost(req, data, len);
        }
    );

    _server.on("/api/firmware-info", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleFirmwareInfo(req);
    });

    // ── Backup / restore ─────────────────────────────────────
    // The download serializes a single JSON document with every NVS
    // namespace + on-disk file the console owns. The restore endpoint
    // accepts the same shape and writes everything back. Both are
    // gated by _requireAuth: the backup contains every secret the
    // device knows.
    _server.on("/api/backup", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        _handleBackupGet(req);
    });
    _server.on("/api/restore", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            // Final response handler — fires after every body chunk has
            // been delivered. The actual JSON parse + apply runs here
            // so we can return a real status code instead of a vague
            // "upload accepted" 200.
            if (!_restoreReady) {
                req->send(400, "application/json",
                          "{\"error\":\"upload incomplete or rejected\"}");
                return;
            }
            JsonDocument doc;
            DeserializationError jerr = deserializeJson(doc, _restoreBuffer);
            _restoreBuffer = String();
            _restoreReady  = false;
            if (jerr) {
                req->send(400, "application/json",
                          "{\"error\":\"backup is not valid JSON\"}");
                return;
            }
            String why;
            if (!SpoolhardBackup::validate(doc, "console", why)) {
                String body = String("{\"error\":\"") + why + "\"}";
                req->send(400, "application/json", body);
                return;
            }
            // Ownership list — every namespace the console writes to.
            // Restore only touches namespaces in this list, so a tampered
            // or sibling-product backup can't poison unrelated NVS state.
            SpoolhardBackup::Source src;
            src.nvs_namespaces = {
                NVS_NS_WIFI, NVS_NS_SCALE, NVS_NS_PRINTERS, NVS_NS_DISPLAY,
                NVS_NS_CORE_WEIGHTS, NVS_NS_CONSOLE, NVS_NS_STORE,
                NVS_NS_BAMBU_CLOUD, "ota_cfg",
            };
            // userfs (LittleFS) holds /spools.jsonl. SD holds user-managed
            // filaments + printers config. Skip the stock filaments DB —
            // it's a release artifact, regenerated by the build pipeline.
            SpoolhardBackup::FsMount lfs{"littlefs", &LittleFS, /*max*/ 0, {}};
            SpoolhardBackup::FsMount sdm{"sd", &SD, /*max*/ 0, { "/filaments.jsonl", "/filaments.db", "/STORE", "/STATE" }};
            src.fs_mounts = { lfs, sdm };
            SpoolhardBackup::RestoreReport rep;
            bool ok = SpoolhardBackup::applyRestore(src, doc, rep);
            JsonDocument resp;
            resp["ok"]               = ok;
            resp["nvs_keys_set"]     = rep.nvs_keys_set;
            resp["nvs_keys_skipped"] = rep.nvs_keys_skipped;
            resp["files_written"]    = rep.files_written;
            resp["files_skipped"]    = rep.files_skipped;
            resp["errors"]           = rep.errors;
            if (!rep.first_error.isEmpty()) resp["first_error"] = rep.first_error;
            String body; serializeJson(resp, body);
            req->send(ok ? 200 : 500, "application/json", body);
            // Reboot ~1s later so the response actually flushes. Skip
            // reboot if nothing landed (e.g. zero-key file) so the user
            // can investigate without losing the console. Same
            // delay-then-restart pattern as the upload routes above.
            if (ok && (rep.nvs_keys_set || rep.files_written)) {
                delay(1000); ESP.restart();
            }
        },
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len,
               size_t index, size_t total) {
            (void)req;
            _handleRestorePost(req, data, len, index, total);
        }
    );

    // ── Spool CRUD ───────────────────────────────────────────
    _server.on("/api/spools", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleSpoolsList(req);
    });
    _server.on("^\\/api\\/spools\\/(.+)$", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleSpoolGet(req);
    });
    _server.on("/api/spools", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleSpoolPost(req, data, len);
        }
    );
    _server.on("^\\/api\\/spools\\/(.+)$", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        _handleSpoolDelete(req);
    });

    // ── User filament CRUD + cloud sync ──────────────────────
    //
    // IMPORTANT: register the specific cloud-sync / cloud-push routes
    // FIRST. ESPAsyncWebServer matches in registration order and the
    // generic `/api/user-filaments` POST below is a prefix that
    // swallows `/api/user-filaments/cloud-sync` — its onRequest is an
    // empty lambda (the body parser does the work) so a request sent
    // here without a body would just sit until the connection drops.
    // Same mistake the comment on the printers section warns about.
    _server.on("/api/user-filaments/cloud-sync", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        _handleUserFilamentsCloudSync(req);
    });
    _server.on("/api/user-filaments/cloud-sync/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleUserFilamentsCloudSyncStatus(req);
    });
    _server.on("^\\/api\\/user-filaments\\/(.+)\\/cloud-push$", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        _handleUserFilamentCloudPush(req);
    });

    _server.on("/api/user-filaments", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleUserFilamentsList(req);
    });
    _server.on("^\\/api\\/user-filaments\\/(.+)$", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleUserFilamentGet(req);
    });
    _server.on("/api/user-filaments", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleUserFilamentPost(req, data, len);
        }
    );
    _server.on("^\\/api\\/user-filaments\\/(.+)$", HTTP_PUT, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleUserFilamentPost(req, data, len);  // PUT shares the upsert handler
        }
    );
    _server.on("^\\/api\\/user-filaments\\/(.+)$", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        _handleUserFilamentDelete(req);
    });

    // ── Printers CRUD ────────────────────────────────────────
    // Register specific sub-routes FIRST — the ESPAsyncWebServer regex router
    // matches in registration order and `^/api/printers/(.+)$` is greedy
    // enough to swallow `/api/printers/<serial>/analysis` as a single capture
    // group, shadowing the /analysis, /analyze, and /ams-mapping handlers.
    _server.on("^\\/api\\/printers\\/([^/]+)\\/analyze$", HTTP_POST,
        [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handlePrinterAnalyzeStart(req, data, len);
        });
    _server.on("^\\/api\\/printers\\/([^/]+)\\/analysis$", HTTP_GET,
        [this](AsyncWebServerRequest* req) {
            _handlePrinterAnalysisGet(req);
        });
    _server.on("^\\/api\\/printers\\/([^/]+)\\/ams-mapping$", HTTP_POST,
        [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handlePrinterAmsMappingPost(req, data, len);
        });
    // Interactive FTP debug: starts a background task that runs the requested
    // operation (probe / list / download) and streams per-step progress over
    // the WS as ftp_trace + a final ftp_done event. Body is JSON
    // {"op": "probe"|"list"|"download", "path": "/cache"}.
    _server.on("^\\/api\\/printers\\/([^/]+)\\/ftp-debug$", HTTP_POST,
        [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handlePrinterFtpDebug(req, data, len);
        });

    _server.on("/api/printers", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handlePrintersList(req);
    });
    _server.on("^\\/api\\/printers\\/(.+)$", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handlePrinterGet(req);
    });
    _server.on("/api/printers", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handlePrinterPost(req, data, len);
        }
    );
    _server.on("^\\/api\\/printers\\/(.+)$", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        _handlePrinterDelete(req);
    });
    _server.on("/api/discovery/printers", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleDiscoveryPrinters(req);
    });
    // Core-weights DB (auto-learned empty-spool reference values).
    _server.on("/api/core-weights", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleCoreWeightsGet(req);
    });
    _server.on("/api/core-weights", HTTP_PUT,
        [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleCoreWeightsPut(req, data, len);
        });
    _server.on("/api/core-weights", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        _handleCoreWeightsDelete(req);
    });

    // Quick-weights shortcut list for the wizard "Full" step.
    _server.on("/api/quick-weights", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleQuickWeightsGet(req);
    });
    _server.on("/api/quick-weights", HTTP_POST,
        [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleQuickWeightsPost(req, data, len);
        });
    _server.on("/api/display-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleDisplayConfigGet(req);
    });
    _server.on("/api/display-config", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            _handleDisplayConfigPost(req, data, len);
        });

    _server.on("/api/discovery/scales", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleDiscoveryScales(req);
    });

    // ── Scale-link forwarding ────────────────────────────────
    _server.on("/api/scale-link", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleScaleLinkStatus(req);
    });
    _server.on("/api/scale-link/tare", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (_scale) _scale->tare();
        req->send(200, "application/json", "{\"ok\":true}");
    });
    _server.on("/api/scale-link/calibrate", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            int32_t w = doc["weight"] | 0;
            if (w <= 0) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"weight must be > 0\"}");
                return;
            }
            if (_scale) _scale->calibrate(w);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );
    _server.on("/api/scale-link/read-tag", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (_scale) _scale->readTag();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // Shared secret between console and its paired scale (future HMAC use).
    _server.on("/api/scale-secret", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleScaleSecretGet(req);
    });
    _server.on("/api/scale-secret", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            _handleScaleSecretPost(req, data, len);
        }
    );

    // ── Bambu Lab cloud auth ────────────────────────────────
    _server.on("/api/bambu-cloud", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleBambuCloudGet(req);
    });
    _server.on("/api/bambu-cloud", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        _handleBambuCloudClear(req);
    });
    _server.on("/api/bambu-cloud/login", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            _handleBambuCloudLogin(req, data, len);
        }
    );
    _server.on("/api/bambu-cloud/login-code", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            _handleBambuCloudLoginCode(req, data, len);
        }
    );
    _server.on("/api/bambu-cloud/login-tfa", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            _handleBambuCloudLoginTfa(req, data, len);
        }
    );
    _server.on("/api/bambu-cloud/token", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            _handleBambuCloudSetToken(req, data, len);
        }
    );
    _server.on("/api/bambu-cloud/verify", HTTP_POST, [this](AsyncWebServerRequest* req) {
        _handleBambuCloudVerify(req);
    });

    // ── Firmware upload (recovery path) ──────────────────────
    _server.on("/api/upload/firmware", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!_uploadAccepted) {
                const char* reason = _uploadRejectReason ? _uploadRejectReason : "unknown";
                Serial.printf("[Upload] Firmware response 400: %s\n", reason);
                char body[160];
                snprintf(body, sizeof(body),
                         "{\"ok\":false,\"error\":\"upload rejected (%s) — expected "
                         PRODUCT_NAME " firmware\"}", reason);
                req->send(400, "application/json", body);
                return;
            }
            Serial.println("[Upload] Firmware accepted — rebooting");
            req->send(200, "application/json", "{\"ok\":true}");
            delay(1000); ESP.restart();
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                if (!_requireAuth(req)) {
                    _uploadAccepted = false;
                    _uploadRejectReason = "unauthorized";
                    Update.abort();
                    return;
                }
                Serial.printf("[Upload] Firmware begin: %s\n", filename.c_str());
                // Default to rejected — only the full-success path below flips
                // this true. Guards against partial uploads, dropped
                // connections, and missing `final` chunks.
                _uploadAccepted = false;
                _uploadRejectReason = "incomplete upload";
                _uploadMatcher.reset();
                _uploadVersion.reset();
                _uploadContentLength = req->contentLength();
                _uploadLastReportedPct = -1;
                // First-pass label: just the filename. Will be replaced with
                // the version string once app_desc is parsed (a handful of
                // chunks later).
                _uploadLabel = filename;
                if (_onUploadStarted) _onUploadStarted("firmware");
                if (_onUploadProgress) _onUploadProgress(0, "firmware", _uploadLabel.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Serial.printf("[Upload] Begin failed: %s\n", Update.errorString());
                    _uploadRejectReason = "Update.begin failed";
                    return;
                }
            }
            _uploadMatcher.feed(data, len);
            // Upgrade the label to "v<version>" as soon as the header is in.
            bool versionWasParsed = _uploadVersion.parsed;
            _uploadVersion.feed(data, len);
            if (!versionWasParsed && _uploadVersion.parsed && _uploadVersion.version[0]) {
                _uploadLabel = String("v") + _uploadVersion.version;
                Serial.printf("[Upload] Firmware version detected: %s\n", _uploadLabel.c_str());
            }
            if (Update.isRunning()) Update.write(data, len);

            // Throttle progress updates to every 5% (or the final chunk).
            if (_onUploadProgress && _uploadContentLength > 0) {
                int pct = (int)((uint64_t)(index + len) * 100 / _uploadContentLength);
                if (pct > 100) pct = 100;
                if (pct - _uploadLastReportedPct >= 5 || final) {
                    _uploadLastReportedPct = pct;
                    _onUploadProgress(pct, "firmware", _uploadLabel.c_str());
                }
            }

            if (final) {
                Serial.printf("[Upload] Firmware final: %u bytes, signature %s\n",
                              (unsigned)(index + len),
                              _uploadMatcher.matched() ? "OK" : "MISSING");
                if (!_uploadMatcher.matched()) {
                    Serial.println("[Upload] Firmware rejected: product signature missing");
                    Update.abort();
                    _uploadRejectReason = "wrong product";
                    return;
                }
                if (!Update.end(true)) {
                    Serial.printf("[Upload] Firmware commit failed: %s\n", Update.errorString());
                    _uploadRejectReason = "commit failed";
                    return;
                }
                _uploadAccepted = true;
            }
        }
    );

    // ── Frontend upload (web UI served from SPIFFS) ──────────
    // Path stays /api/upload/spiffs so older uploaders still work; only the
    // release filename and user-visible labels are renamed.
    _server.on("/api/upload/spiffs", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!_uploadAccepted) {
                const char* reason = _uploadRejectReason ? _uploadRejectReason : "unknown";
                Serial.printf("[Upload] Frontend response 400: %s\n", reason);
                char body[160];
                snprintf(body, sizeof(body),
                         "{\"ok\":false,\"error\":\"upload rejected (%s) — expected "
                         PRODUCT_NAME " frontend\"}", reason);
                req->send(400, "application/json", body);
                return;
            }
            Serial.println("[Upload] Frontend accepted — rebooting");
            req->send(200, "application/json", "{\"ok\":true}");
            delay(1000); ESP.restart();
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                if (!_requireAuth(req)) {
                    _uploadAccepted = false;
                    _uploadRejectReason = "unauthorized";
                    Update.abort();
                    return;
                }
                Serial.printf("[Upload] Frontend begin: %s\n", filename.c_str());
                _uploadAccepted = false;
                _uploadRejectReason = "incomplete upload";
                _uploadMatcher.reset();
                _uploadVersion.reset();
                _uploadContentLength = req->contentLength();
                _uploadLastReportedPct = -1;
                _uploadLabel = filename;
                if (_onUploadStarted) _onUploadStarted("spiffs");
                if (_onUploadProgress) _onUploadProgress(0, "spiffs", _uploadLabel.c_str());
                if (!Update.begin(0x300000, U_SPIFFS)) {
                    Serial.printf("[Upload] Begin failed: %s\n", Update.errorString());
                    _uploadRejectReason = "Update.begin failed";
                    return;
                }
            }
            _uploadMatcher.feed(data, len);
            // Scan the SPIFFS stream for the same marker we plant in the
            // firmware binary — release.sh writes a plain-text file into
            // the data/ dir so the raw bytes end up in the mkspiffs image.
            bool versionWasParsed = _uploadVersion.parsed;
            _uploadVersion.feed(data, len);
            if (!versionWasParsed && _uploadVersion.parsed && _uploadVersion.version[0]) {
                _uploadLabel = String("v") + _uploadVersion.version;
                Serial.printf("[Upload] Frontend version detected: %s\n", _uploadLabel.c_str());
            }
            if (Update.isRunning()) Update.write(data, len);

            if (_onUploadProgress && _uploadContentLength > 0) {
                int pct = (int)((uint64_t)(index + len) * 100 / _uploadContentLength);
                if (pct > 100) pct = 100;
                if (pct - _uploadLastReportedPct >= 5 || final) {
                    _uploadLastReportedPct = pct;
                    _onUploadProgress(pct, "spiffs", _uploadLabel.c_str());
                }
            }

            if (final) {
                Serial.printf("[Upload] Frontend final: %u bytes, signature %s\n",
                              (unsigned)(index + len),
                              _uploadMatcher.matched() ? "OK" : "MISSING");
                if (!_uploadMatcher.matched()) {
                    // SPIFFS bytes have already landed in the partition; wipe
                    // it so next boot mounts clean rather than trying to read
                    // a half-overwritten wrong-product image.
                    Serial.println("[Upload] Frontend rejected: product signature missing");
                    Update.abort();
                    SPIFFS.format();
                    _uploadRejectReason = "wrong product";
                    return;
                }
                if (!Update.end(true)) {
                    Serial.printf("[Upload] Frontend commit failed: %s\n", Update.errorString());
                    _uploadRejectReason = "commit failed";
                    return;
                }
                _uploadAccepted = true;
            }
        }
    );

    // ── Filaments library (SD-resident, flat JSONL) ──────────
    // User ships a `filaments.jsonl` generated by the build pipeline
    // (scripts/build_filaments_db.sh — name kept for CI compat). Each
    // line is one resolved filament preset; the firmware parses them
    // into RAM at boot via StockFilamentsStore so the LCD picker can
    // show stock filaments alongside user-created ones, and the React
    // frontend pulls the same file via GET /api/filaments and parses
    // it client-side (no sql.js, no SQLite client on-device).
    //
    // Path is relative to the SD root: Arduino's VFS wrapper prepends
    // the mountpoint (SD_MOUNT = "/sd") internally, so passing
    // "/sd/..." here would collide into "/sd/sd/..." and fail to open.
    static constexpr const char* FILAMENTS_RELPATH = "/filaments.jsonl";

    _server.on("/api/upload/filaments", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!_filamentsUploadOk) {
                char body[160];
                snprintf(body, sizeof(body),
                         "{\"ok\":false,\"error\":\"%s\"}",
                         _filamentsUploadErr ? _filamentsUploadErr : "unknown");
                req->send(400, "application/json", body);
                return;
            }
            char body[96];
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"size\":%u}", (unsigned)_filamentsUploadBytes);
            req->send(200, "application/json", body);
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
               uint8_t* data, size_t len, bool final) {
            // NOTE: intentionally do NOT fire _onUploadStarted /
            // _onUploadProgress here. Those callbacks are wired straight to
            // ui_show_ota_progress() in main.cpp — which takes over the LCD
            // with the "Updating firmware" screen and never transitions
            // back. This is a user-data upload, not a firmware flash; the
            // browser's DropZone already shows its own XHR-driven progress
            // bar, and the LCD stays put on whatever screen was active.
            if (index == 0) {
                _filamentsUploadOk    = false;
                _filamentsUploadErr   = "incomplete upload";
                _filamentsUploadBytes = 0;
                if (!_requireAuth(req)) { _filamentsUploadErr = "unauthorized"; return; }
                if (!g_sd.isMounted()) { _filamentsUploadErr = "SD card not mounted"; return; }
                Serial.printf("[Upload] filaments.db begin: %s\n", filename.c_str());
                if (_filamentsUploadFile) _filamentsUploadFile.close();
                _filamentsUploadFile = SD.open(FILAMENTS_RELPATH, FILE_WRITE);
                if (!_filamentsUploadFile) { _filamentsUploadErr = "SD open for write failed"; return; }
            }
            if (_filamentsUploadFile) {
                size_t w = _filamentsUploadFile.write(data, len);
                if (w != len) {
                    _filamentsUploadErr = "SD write short -- card full?";
                    _filamentsUploadFile.close();
                    SD.remove(FILAMENTS_RELPATH);
                    return;
                }
                _filamentsUploadBytes += w;
            }
            if (final) {
                if (_filamentsUploadFile) {
                    _filamentsUploadFile.flush();
                    _filamentsUploadFile.close();
                }
                Serial.printf("[Upload] filaments.jsonl final: %u bytes\n",
                              (unsigned)_filamentsUploadBytes);
                _filamentsUploadOk = (_filamentsUploadBytes > 0);
                if (!_filamentsUploadOk) {
                    _filamentsUploadErr = "zero-length upload";
                } else {
                    // Reload the in-memory stock-filament cache so the
                    // new file takes effect without a reboot — the LCD
                    // wizard's New Filament picker will see the new
                    // rows on next open.
                    g_stock_filaments.reload();
                }
            }
        }
    );

    // IMPORTANT: AsyncWebServer matches routes prefix-style (registered uri
    // + "/" against the incoming url), so `/api/filaments` silently
    // swallows `/api/filaments/info` unless the more-specific route is
    // registered first. Earlier ordering caused the info endpoint to stream
    // the actual .db file instead of JSON, which the frontend parsed as an
    // empty object → "No library uploaded yet" even after a clean upload.
    _server.on("/api/filaments/info", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        JsonDocument doc;
        doc["present"]    = false;
        doc["sd_mounted"] = g_sd.isMounted();
        if (g_sd.isMounted() && SD.exists(FILAMENTS_RELPATH)) {
            File f = SD.open(FILAMENTS_RELPATH, FILE_READ);
            if (f) {
                doc["present"] = true;
                doc["size"]    = (uint32_t)f.size();
                doc["mtime_s"] = (uint32_t)f.getLastWrite();   // FAT mtime, seconds since epoch
                f.close();
            }
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    _server.on("/api/filaments", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (!g_sd.isMounted() || !SD.exists(FILAMENTS_RELPATH)) {
            req->send(404, "application/json", "{\"error\":\"no library uploaded\"}");
            return;
        }
        // Content type: JSON Lines per RFC 8259 / IANA convention. Some
        // clients are happier with text/plain; we return JSONL's own
        // type so the React fetch() side can branch on it cleanly.
        req->send(SD, FILAMENTS_RELPATH, "application/x-ndjson");
    });

    // Downloads the blob the most recent FTP debug "download" op wrote to
    // /sd/ftp_dl.bin. Auth-gated, served with attachment disposition so the
    // browser saves it rather than trying to render unknown bytes.
    _server.on("/api/ftp-download", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (!g_sd.isMounted() || !SD.exists(SD_MOUNT "/ftp_dl.bin")) {
            req->send(404, "application/json", "{\"error\":\"no download available\"}");
            return;
        }
        auto* resp = req->beginResponse(SD, SD_MOUNT "/ftp_dl.bin",
                                        "application/octet-stream");
        resp->addHeader("Content-Disposition", "attachment; filename=\"ftp_dl.bin\"");
        req->send(resp);
    });

    _server.on("/api/filaments", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        if (!g_sd.isMounted()) {
            req->send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
            return;
        }
        if (!SD.exists(FILAMENTS_RELPATH)) {
            req->send(404, "application/json", "{\"error\":\"no library uploaded\"}");
            return;
        }
        if (!SD.remove(FILAMENTS_RELPATH)) {
            req->send(500, "application/json", "{\"error\":\"delete failed\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ── Debug toggles (session-only) ─────────────────────────
    _server.on("/api/debug/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!_requireAuth(req)) return;
        JsonDocument doc;
        doc["log_ams"] = _logAmsRaw;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
    _server.on("/api/debug/config", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!_requireAuth(req)) return;
            JsonDocument body;
            if (deserializeJson(body, data, len)) {
                req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
                return;
            }
            // Only patch keys that are present — leaves other flags alone if
            // we add more later.
            if (body["log_ams"].is<bool>()) _logAmsRaw = body["log_ams"].as<bool>();
            JsonDocument resp; resp["log_ams"] = _logAmsRaw;
            String out; serializeJson(resp, out);
            req->send(200, "application/json", out);
        }
    );

    // ── Static SPA from SPIFFS ───────────────────────────────
    // Root: serve the gzipped SPA index explicitly so we don't depend on
    // serveStatic's auto-gzip variant handling.
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.printf("[HTTP] GET / from %s\n", req->client()->remoteIP().toString().c_str());
        if (SPIFFS.exists("/index.html.gz")) {
            auto* resp = req->beginResponse(SPIFFS, "/index.html.gz", "text/html");
            resp->addHeader("Content-Encoding", "gzip");
            req->send(resp);
        } else if (SPIFFS.exists("/index.html")) {
            req->send(SPIFFS, "/index.html", "text/html");
        } else {
            req->send(200, "text/html",
                "<h1>SpoolHard Console</h1>"
                "<p>Frontend not installed. Upload via /api/upload/spiffs.</p>");
        }
    });

    // Serve static assets. Auto-gzip support: if a `.gz` variant exists in
    // SPIFFS, serveStatic serves it with Content-Encoding: gzip.
    _server.serveStatic("/", SPIFFS, "/")
           .setDefaultFile("index.html")
           .setCacheControl("max-age=86400");

    // Log every incoming request that isn't handled by a registered route.
    // Helps diagnose 404s from the SPA or missing assets.
    _server.on("/__debug/list", HTTP_GET, [](AsyncWebServerRequest* req) {
        String out = "SPIFFS files:\n";
        File root = SPIFFS.open("/");
        File f = root.openNextFile();
        while (f) {
            out += String(f.path()) + "  " + String(f.size()) + "\n";
            f = root.openNextFile();
        }
        req->send(200, "text/plain", out);
    });

    _server.onNotFound([](AsyncWebServerRequest* req) {
        Serial.printf("[HTTP] 404-fallback %s %s from %s\n",
                      req->methodToString(), req->url().c_str(),
                      req->client()->remoteIP().toString().c_str());
        if (req->method() == HTTP_GET &&
            !req->url().startsWith("/api/") &&
            !req->url().startsWith("/captive")) {
            if (SPIFFS.exists("/index.html.gz")) {
                auto* resp = req->beginResponse(SPIFFS, "/index.html.gz", "text/html");
                resp->addHeader("Content-Encoding", "gzip");
                req->send(resp);
            } else if (SPIFFS.exists("/index.html")) {
                req->send(SPIFFS, "/index.html", "text/html");
            } else {
                req->send(200, "text/html",
                    "<h1>SpoolHard Console</h1>"
                    "<p>Frontend not installed. Upload via /api/upload/spiffs.</p>");
            }
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });
}

// ── Handlers ─────────────────────────────────────────────────

void ConsoleWebServer::_handleDeviceName(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String name = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardConsole");
    prefs.end();
    JsonDocument doc; doc["device_name"] = name;
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

void ConsoleWebServer::_handleWifiStatus(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    bool configured = prefs.isKey(NVS_KEY_SSID);
    String ssid     = prefs.getString(NVS_KEY_SSID, "");
    prefs.end();

    bool connected = (WiFi.status() == WL_CONNECTED);
    JsonDocument doc;
    doc["configured"] = configured && !ssid.isEmpty();
    doc["connected"]  = connected;
    doc["ssid"]       = connected ? WiFi.SSID() : ssid;
    doc["ip"]         = connected ? WiFi.localIP().toString() : "";
    doc["rssi"]       = connected ? WiFi.RSSI() : 0;
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

void ConsoleWebServer::_handleOtaConfigGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    OtaConfig cfg; cfg.load();
    JsonDocument doc;
    doc["url"]               = cfg.url;
    doc["use_ssl"]           = cfg.use_ssl;
    doc["verify_ssl"]        = cfg.verify_ssl;
    doc["check_enabled"]     = cfg.check_enabled;
    doc["check_interval_h"]  = cfg.check_interval_h;
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

void ConsoleWebServer::_handleOtaConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    OtaConfig cfg; cfg.load();
    if (doc["url"].is<const char*>())     cfg.url        = doc["url"].as<String>();
    if (doc["use_ssl"].is<bool>())        cfg.use_ssl    = doc["use_ssl"];
    if (doc["verify_ssl"].is<bool>())     cfg.verify_ssl = doc["verify_ssl"];
    if (doc["check_enabled"].is<bool>())  cfg.check_enabled    = doc["check_enabled"];
    if (doc["check_interval_h"].is<uint32_t>()) {
        uint32_t h = doc["check_interval_h"];
        if (h < kOtaCheckIntervalMin) h = kOtaCheckIntervalMin;
        if (h > kOtaCheckIntervalMax) h = kOtaCheckIntervalMax;
        cfg.check_interval_h = h;
    }
    if (!cfg.use_ssl) cfg.verify_ssl = false;
    cfg.save();

    JsonDocument r;
    r["url"]              = cfg.url;
    r["use_ssl"]          = cfg.use_ssl;
    r["verify_ssl"]       = cfg.verify_ssl;
    r["check_enabled"]    = cfg.check_enabled;
    r["check_interval_h"] = cfg.check_interval_h;
    String s; serializeJson(r, s);
    req->send(200, "application/json", s);
}

// Surface what the periodic checker has learned: the timestamp of the
// last attempt, its tag, the latest-known versions for each component
// the device tracks, and a pending-updates summary the React side
// (and later the LCD banner) can render. Doesn't touch the network —
// it's all in-memory cache from the last check.
void ConsoleWebServer::_handleOtaStatus(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    OtaConfig cfg; cfg.load();
    auto p = g_ota_checker.pending();

    JsonDocument doc;
    doc["check_enabled"]    = cfg.check_enabled;
    doc["check_interval_h"] = cfg.check_interval_h;
    doc["last_check_ts"]    = g_ota_checker.lastCheckTs();
    doc["last_check_status"] = g_ota_checker.lastStatus();
    doc["check_in_flight"]  = g_ota_checker.checkInFlight();

    JsonObject console = doc["console"].to<JsonObject>();
    console["firmware_current"] = p.firmware_current;
    console["firmware_latest"]  = p.firmware_latest;
    console["frontend_current"] = p.frontend_current;
    console["frontend_latest"]  = p.frontend_latest;
    console["pending"]          = p.firmware || p.frontend;
    if (g_ota_in_flight.valid) {
        JsonObject ip = console["in_progress"].to<JsonObject>();
        ip["kind"]    = g_ota_in_flight.kind;
        ip["percent"] = g_ota_in_flight.percent;
    }

    // Scale block: link state and the cached OtaPending snapshot are
    // reported independently. Earlier code only emitted version fields
    // when (linkUp && sop.valid), which had the awkward effect of
    // showing "unavailable" + "scale waiting" during the brief window
    // between WS reconnect and the scale's first push — and on every
    // link flap. Now: link reflects the live WS state; versions come
    // from the cache whenever it's been seeded at least once. The
    // cache survives disconnects (see ScaleLink::_markDisconnected) so
    // the UI keeps the last-known snapshot until a fresh one arrives.
    JsonObject scale = doc["scale"].to<JsonObject>();
    bool linkUp = _scale && _scale->isConnected();
    const auto& sop = _scale ? _scale->scaleOtaPending() : ScaleLink::ScaleOtaPending{};
    scale["link"] = linkUp ? (sop.valid ? "online" : "waiting") : "offline";
    if (sop.valid) {
        scale["firmware_current"]  = sop.firmware_current;
        scale["firmware_latest"]   = sop.firmware_latest;
        scale["frontend_current"]  = sop.frontend_current;
        scale["frontend_latest"]   = sop.frontend_latest;
        scale["last_check_ts"]     = sop.last_check_ts;
        scale["last_check_status"] = sop.last_check_status;
        scale["pending"]           = sop.firmware_update || sop.frontend_update;
    } else {
        scale["pending"] = false;
    }
    if (_scale) {
        const auto& sif = _scale->scaleOtaInFlight();
        if (sif.valid) {
            JsonObject ip = scale["in_progress"].to<JsonObject>();
            ip["kind"]    = sif.kind;
            ip["percent"] = sif.percent;
        }
    }

    String s; serializeJson(doc, s);
    req->send(200, "application/json", s);
}

void ConsoleWebServer::_handleReset(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    req->send(200, "application/json", "{\"status\":\"resetting\"}");
    delay(500);
    ESP.restart();
}

void ConsoleWebServer::_handleTestKey(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    bool configured = prefs.isKey(NVS_KEY_FIXED_KEY);
    String key = prefs.getString(NVS_KEY_FIXED_KEY, DEFAULT_FIXED_KEY);
    prefs.end();

    String masked = key;
    if (key.length() > 4) masked = key.substring(0, 2) + "***" + key.substring(key.length() - 2);

    JsonDocument doc;
    doc["configured"]  = configured;
    doc["key_preview"] = masked;
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

void ConsoleWebServer::_handleFixedKeyConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
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
    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handleFirmwareInfo(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    doc["fw_version"]   = FW_VERSION;
    doc["fe_version"]   = FE_VERSION;
    doc["flash_size"]   = ESP.getFlashChipSize();
    doc["spiffs_total"] = SPIFFS.totalBytes();
    doc["spiffs_used"]  = SPIFFS.usedBytes();
    doc["userfs_total"] = LittleFS.totalBytes();
    doc["userfs_used"]  = LittleFS.usedBytes();
    doc["free_heap"]    = ESP.getFreeHeap();
    doc["psram_free"]   = ESP.getFreePsram();
    doc["sd_mounted"]   = g_sd.isMounted();
    doc["sd_total"]     = (double)g_sd.totalBytes();  // 64-bit → JSON double
    doc["sd_used"]      = (double)g_sd.usedBytes();
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

// ── Backup / restore ─────────────────────────────────────────

void ConsoleWebServer::_handleBackupGet(AsyncWebServerRequest* req) {
    // Read the device name once for the metadata header. Same source the
    // OTA + WiFi UI reads from.
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String device = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardConsole");
    prefs.end();

    SpoolhardBackup::Source src;
    src.nvs_namespaces = {
        NVS_NS_WIFI, NVS_NS_SCALE, NVS_NS_PRINTERS, NVS_NS_DISPLAY,
        NVS_NS_CORE_WEIGHTS, NVS_NS_CONSOLE, NVS_NS_STORE,
        NVS_NS_BAMBU_CLOUD, "ota_cfg",
    };
    SpoolhardBackup::FsMount lfs{"littlefs", &LittleFS, /*max*/ 0, {}};
    SpoolhardBackup::FsMount sdm{"sd", &SD, /*max*/ 0, { "/filaments.jsonl", "/filaments.db", "/STORE", "/STATE" }};
    src.fs_mounts = { lfs, sdm };

    JsonDocument doc;
    SpoolhardBackup::buildBackup(src, "console", device, FW_VERSION, doc, nullptr);

    String body;
    serializeJson(doc, body);

    // The downloaded file should land with a timestamped sensible name.
    // Browsers honour Content-Disposition's filename= param.
    char filename[96];
    snprintf(filename, sizeof(filename),
             "attachment; filename=\"spoolhard-console-%s-backup.json\"",
             device.c_str());
    AsyncWebServerResponse* resp =
        req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition", filename);
    req->send(resp);
}

void ConsoleWebServer::_handleRestorePost(AsyncWebServerRequest* req,
                                          uint8_t* data, size_t len,
                                          size_t index, size_t total) {
    if (index == 0) {
        if (!_requireAuth(req)) {
            _restoreReady = false;
            _restoreError = "unauthorized";
            return;
        }
        _restoreBuffer = String();
        _restoreReady  = false;
        _restoreError  = "";
        // Reserve up-front when we know the size — saves a few reallocs
        // on big payloads. Bounded by client behaviour; PSRAM keeps the
        // ceiling generous.
        if (total > 0 && total < 2 * 1024 * 1024) {
            _restoreBuffer.reserve(total);
        }
    }
    if (len > 0) _restoreBuffer.concat((const char*)data, len);
    if (index + len >= total) {
        _restoreReady = true;
    }
}

// ── Spool CRUD ───────────────────────────────────────────────

void ConsoleWebServer::_handleSpoolsList(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!_store) { req->send(503, "application/json", "{\"error\":\"store unavailable\"}"); return; }
    int offset = 0, limit = 50;
    String material;
    if (req->hasParam("offset"))   offset = req->getParam("offset")->value().toInt();
    if (req->hasParam("limit"))    limit  = req->getParam("limit")->value().toInt();
    if (req->hasParam("material")) material = req->getParam("material")->value();
    if (limit <= 0 || limit > 200) limit = 50;

    auto rows = _store->list(offset, limit, material);

    JsonDocument doc;
    doc["total"]  = (unsigned)_store->count();
    doc["offset"] = offset;
    doc["limit"]  = limit;
    JsonArray arr = doc["rows"].to<JsonArray>();
    for (auto& r : rows) {
        JsonDocument row; r.toJson(row);
        arr.add(row);
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleSpoolGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!_store) { req->send(503, "application/json", "{\"error\":\"store unavailable\"}"); return; }
    String id = req->pathArg(0);
    SpoolRecord rec;
    if (!_store->findById(id, rec)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    JsonDocument doc; rec.toJson(doc);
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleSpoolPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    if (!_store) { req->send(503, "application/json", "{\"error\":\"store unavailable\"}"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    const char* id_in = doc["id"] | "";
    if (!*id_in) {
        req->send(400, "application/json", "{\"error\":\"id required\"}");
        return;
    }
    // Merge: load existing (if any), then overlay incoming JSON fields.
    // Without this, partial payloads like the frontend's captureWeight
    // (id + weight_current + consumed_since_weight only) wipe every other
    // field back to defaults — losing nozzle_temp_min/max, slicer_filament,
    // note, etc. fromJson's overlay semantics do the right thing here.
    SpoolRecord rec;
    _store->findById(String(id_in), rec);   // ok if it returns false — new record
    if (!rec.fromJson(doc)) {
        req->send(400, "application/json", "{\"error\":\"id required\"}");
        return;
    }
    if (!_store->upsert(rec)) {
        req->send(500, "application/json", "{\"error\":\"upsert failed\"}");
        return;
    }
    if (!rec.brand.isEmpty() && !rec.material_type.isEmpty() &&
        rec.weight_advertised > 0 && rec.weight_core >= 0) {
        CoreWeights::set(rec.brand, rec.material_type, rec.weight_advertised, rec.weight_core);
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handleSpoolDelete(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!_store) { req->send(503, "application/json", "{\"error\":\"store unavailable\"}"); return; }
    String id = req->pathArg(0);
    if (!_store->remove(id)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── User filament CRUD ────────────────────────────────────────
//
// Storage backed by g_user_filaments (JSONL on SD). The frontend merges
// these with the read-only stock filaments.jsonl into a unified picker
// — there's no combined-server-side view.

void ConsoleWebServer::_handleUserFilamentsList(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    int offset = 0, limit = 200;
    String material;
    if (req->hasParam("offset"))   offset = req->getParam("offset")->value().toInt();
    if (req->hasParam("limit"))    limit  = req->getParam("limit")->value().toInt();
    if (req->hasParam("material")) material = req->getParam("material")->value();
    if (limit <= 0 || limit > 500) limit = 200;

    auto rows = g_user_filaments.list(offset, limit, material);
    JsonDocument doc;
    doc["total"]  = (unsigned)g_user_filaments.count();
    doc["offset"] = offset;
    doc["limit"]  = limit;
    JsonArray arr = doc["rows"].to<JsonArray>();
    for (auto& r : rows) {
        JsonDocument row; r.toJson(row);
        arr.add(row);
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleUserFilamentGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String id = req->pathArg(0);
    FilamentRecord rec;
    if (!g_user_filaments.findById(id, rec)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    JsonDocument doc; rec.toJson(doc);
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleUserFilamentPost(AsyncWebServerRequest* req,
                                               uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    // Two paths:
    //   POST /api/user-filaments — create. setting_id is server-generated
    //     (PFUL<hash>). Caller can omit it; if they include one we accept
    //     it (lets a sync flow recreate a known id).
    //   PUT  /api/user-filaments/{id} — update. id from the URL takes
    //     precedence; body's setting_id (if any) is ignored.
    String pathId = req->pathArg(0);   // "" for POST, id for PUT
    String bodyId = doc["setting_id"] | "";

    FilamentRecord rec;
    String targetId = pathId.length() ? pathId : bodyId;
    if (targetId.length()) {
        // Load existing (if any) so a partial PUT doesn't wipe untouched
        // fields. Mirrors the spool POST overlay semantics.
        g_user_filaments.findById(targetId, rec);
    }
    // Ensure setting_id is set BEFORE fromJson runs — the validator
    // requires a non-empty id, but for fresh POSTs we generate it
    // server-side (the body doesn't include one). Without this seeding,
    // every "create new filament" POST 400'd with "invalid filament JSON"
    // because rec was default-constructed with an empty id and the body
    // had nothing to overlay.
    if (rec.setting_id.isEmpty()) {
        rec.setting_id = pathId.length() ? pathId
                       : bodyId.length() ? bodyId
                       :                   UserFilamentsStore::newLocalId();
    }
    if (!rec.fromJson(doc)) {
        req->send(400, "application/json", "{\"error\":\"invalid filament JSON\"}");
        return;
    }
    if (pathId.length()) rec.setting_id = pathId;
    if (rec.updated_at == 0) rec.updated_at = (uint32_t)time(nullptr);

    if (!g_user_filaments.upsert(rec)) {
        req->send(500, "application/json", "{\"error\":\"upsert failed\"}");
        return;
    }
    JsonDocument resp; rec.toJson(resp);
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleUserFilamentDelete(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String id = req->pathArg(0);
    if (!g_user_filaments.remove(id)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── Cloud sync (Phase C) ───────────────────────────────────────
//
// Both handlers degrade gracefully when Cloudflare blocks the device's
// TLS handshake against api.bambulab.com (which is the typical case
// today — the same WAF that blocks login). Status string conventions
// match the BambuCloudFilaments::Result enum so the React UI has one
// switch to render: "ok" / "rejected" / "unreachable".

static const char* _cloudResultStr(BambuCloudFilaments::Result r) {
    switch (r) {
        case BambuCloudFilaments::Result::Ok:          return "ok";
        case BambuCloudFilaments::Result::Rejected:    return "rejected";
        case BambuCloudFilaments::Result::Unreachable: return "unreachable";
    }
    return "rejected";
}

// ── Async cloud-sync state ────────────────────────────────────
//
// AsyncWebServer's request handlers run on the AsyncTCP task. Doing
// many sequential mbedtls handshakes (fetchAll loops one TLS GET per
// preset) inside that task wedges the entire HTTP server until the
// peer gives up + closes the socket.
//
// Fix: launch a one-shot FreeRTOS task that does the fetchAll work,
// store progress + result in a shared struct, and have the HTTP
// handler return immediately. The frontend polls
// `GET /api/user-filaments/cloud-sync/status` for the final result.
struct CloudSyncState {
    enum class Phase { Idle, Running, Done };
    Phase           phase     = Phase::Idle;
    uint32_t        started_at = 0;     // epoch s
    uint32_t        finished_at = 0;
    String          result_status;      // "ok" | "rejected" | "unreachable" (Done only)
    int             added = 0;
    int             updated = 0;
    BambuCloudFilaments::Diag diag;
};
static CloudSyncState  s_cloudSync;
static SemaphoreHandle_t s_cloudSyncMtx = nullptr;

static void _cloudSyncTask(void* /*arg*/) {
    dlog("CloudSync", "task started, free heap=%u", (unsigned)ESP.getFreeHeap());
    std::vector<FilamentRecord> cloudList;
    BambuCloudFilaments::Diag diag;
    auto r = BambuCloudFilaments::fetchAll(g_bambu_cloud.token(),
                                           g_bambu_cloud.region(),
                                           cloudList, &diag);
    dlog("CloudSync", "fetchAll returned status=%s entries=%u",
         _cloudResultStr(r), (unsigned)cloudList.size());

    int added = 0, updated = 0, failed = 0;
    if (r == BambuCloudFilaments::Result::Ok) {
        // Snapshot existing cloud_setting_id → local setting_id once, instead
        // of re-listing the SD-backed store on every preset (was O(N²) and
        // performed a fresh SD read for each comparison — both wasteful and a
        // contention risk against any concurrent write).
        std::map<String, String> existingByCloud;
        for (auto& local : g_user_filaments.list(0, 1000, "")) {
            if (local.cloud_setting_id.length()) {
                existingByCloud[local.cloud_setting_id] = local.setting_id;
            }
        }
        for (auto& cf : cloudList) {
            auto it = existingByCloud.find(cf.cloud_setting_id);
            bool found = (it != existingByCloud.end());
            if (found)  cf.setting_id = it->second;
            else        cf.setting_id = cf.cloud_setting_id;
            cf.updated_at = (uint32_t)time(nullptr);
            bool ok = g_user_filaments.upsert(cf);
            if (!ok) {
                ++failed;
                dlog("CloudSync", "upsert FAILED for %s (%s)",
                     cf.setting_id.c_str(), cf.name.c_str());
            } else if (found) {
                ++updated;
            } else {
                ++added;
            }
        }
    }
    dlog("CloudSync", "reconcile: added=%d updated=%d failed=%d",
         added, updated, failed);

    if (xSemaphoreTake(s_cloudSyncMtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_cloudSync.phase         = CloudSyncState::Phase::Done;
        s_cloudSync.finished_at   = (uint32_t)time(nullptr);
        s_cloudSync.result_status = _cloudResultStr(r);
        s_cloudSync.added         = added;
        s_cloudSync.updated       = updated;
        s_cloudSync.diag          = diag;
        xSemaphoreGive(s_cloudSyncMtx);
    }
    dlog("CloudSync", "task done added=%d updated=%d", added, updated);
    vTaskDelete(nullptr);
}

void ConsoleWebServer::_handleUserFilamentsCloudSync(AsyncWebServerRequest* req) {
    if (!s_cloudSyncMtx) s_cloudSyncMtx = xSemaphoreCreateMutex();
    if (!g_bambu_cloud.haveToken()) {
        req->send(400, "application/json",
                  "{\"error\":\"no Bambu Cloud token configured\"}");
        return;
    }

    bool already_running = false;
    if (xSemaphoreTake(s_cloudSyncMtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_cloudSync.phase == CloudSyncState::Phase::Running) {
            already_running = true;
        } else {
            s_cloudSync.phase       = CloudSyncState::Phase::Running;
            s_cloudSync.started_at  = (uint32_t)time(nullptr);
            s_cloudSync.finished_at = 0;
            s_cloudSync.result_status = "";
            s_cloudSync.added = 0;
            s_cloudSync.updated = 0;
            s_cloudSync.diag = BambuCloudFilaments::Diag{};
        }
        xSemaphoreGive(s_cloudSyncMtx);
    }
    if (already_running) {
        req->send(200, "application/json",
                  "{\"status\":\"running\",\"message\":\"already in progress\"}");
        return;
    }

    // 8 KB stack — fetchAll + per-preset GET + JSON parsing fit
    // comfortably; mbedtls handshake heap is global, not per-task.
    BaseType_t ok = xTaskCreatePinnedToCore(
        _cloudSyncTask, "cloudSync", 8192, nullptr,
        1 /* low prio */, nullptr, APP_CPU_NUM);
    if (ok != pdPASS) {
        if (xSemaphoreTake(s_cloudSyncMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_cloudSync.phase = CloudSyncState::Phase::Idle;
            xSemaphoreGive(s_cloudSyncMtx);
        }
        req->send(500, "application/json",
                  "{\"error\":\"failed to spawn cloud-sync task\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"status\":\"running\",\"message\":\"started — poll /api/user-filaments/cloud-sync/status\"}");
}

void ConsoleWebServer::_handleUserFilamentsCloudSyncStatus(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!s_cloudSyncMtx) s_cloudSyncMtx = xSemaphoreCreateMutex();
    JsonDocument resp;
    if (xSemaphoreTake(s_cloudSyncMtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        const char* phase = "idle";
        if (s_cloudSync.phase == CloudSyncState::Phase::Running) phase = "running";
        if (s_cloudSync.phase == CloudSyncState::Phase::Done)    phase = "done";
        resp["phase"]       = phase;
        resp["started_at"]  = s_cloudSync.started_at;
        resp["finished_at"] = s_cloudSync.finished_at;
        if (s_cloudSync.phase == CloudSyncState::Phase::Done) {
            resp["status"]  = s_cloudSync.result_status;
            resp["added"]   = s_cloudSync.added;
            resp["updated"] = s_cloudSync.updated;
            if (s_cloudSync.result_status != "ok") {
                JsonObject d = resp["diagnostics"].to<JsonObject>();
                d["stage"]         = s_cloudSync.diag.stage;
                d["request_url"]   = s_cloudSync.diag.url;
                d["http_status"]   = s_cloudSync.diag.httpStatus;
                d["cf_blocked"]    = s_cloudSync.diag.cfBlocked;
                d["response_body"] = s_cloudSync.diag.body;
            }
        }
        xSemaphoreGive(s_cloudSyncMtx);
    }
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleUserFilamentCloudPush(AsyncWebServerRequest* req) {
    if (!g_bambu_cloud.haveToken()) {
        req->send(400, "application/json",
                  "{\"error\":\"no Bambu Cloud token configured\"}");
        return;
    }
    String id = req->pathArg(0);
    FilamentRecord rec;
    if (!g_user_filaments.findById(id, rec)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    // The cloud has no PUT/PATCH AND enforces a uniqueness constraint
    // on (account, preset_name). To "update" an existing cloud preset
    // we MUST delete the old one first — otherwise the create lands as
    // HTTP 409 "setting name conflict" and nothing changes. Order is:
    //   1. DELETE old (only if cloud_setting_id is set)
    //   2. CREATE new
    //   3. Persist new cloud_setting_id locally
    // If step 1 fails (transport / 5xx) we abort — pushing on top would
    // 409 again. If step 2 fails after step 1 succeeded we surface the
    // diagnostics; the local record's cloud_setting_id is cleared so
    // the next push attempts a fresh create instead of trying to delete
    // a now-dead id.
    BambuCloudFilaments::Diag diag;
    JsonDocument resp;

    if (rec.cloud_setting_id.length()) {
        auto dr = BambuCloudFilaments::deleteOne(g_bambu_cloud.token(),
                                                  g_bambu_cloud.region(),
                                                  rec.cloud_setting_id, &diag);
        if (dr != BambuCloudFilaments::Result::Ok) {
            resp["status"] = _cloudResultStr(dr);
            JsonObject d = resp["diagnostics"].to<JsonObject>();
            d["stage"]         = diag.stage;
            d["request_url"]   = diag.url;
            d["http_status"]   = diag.httpStatus;
            d["cf_blocked"]    = diag.cfBlocked;
            d["response_body"] = diag.body;
            String out; serializeJson(resp, out);
            req->send(200, "application/json", out);
            return;
        }
        // Optimistically clear locally — even if the upcoming create
        // fails, the cloud-side preset is gone so we mustn't try to
        // delete it again next time.
        rec.cloud_setting_id = "";
        g_user_filaments.upsert(rec);
    }

    String newCloudId;
    auto cr = BambuCloudFilaments::createOne(g_bambu_cloud.token(),
                                              g_bambu_cloud.region(),
                                              rec, newCloudId, &diag);
    resp["status"] = _cloudResultStr(cr);
    if (cr != BambuCloudFilaments::Result::Ok) {
        JsonObject d = resp["diagnostics"].to<JsonObject>();
        d["stage"]         = diag.stage;
        d["request_url"]   = diag.url;
        d["http_status"]   = diag.httpStatus;
        d["cf_blocked"]    = diag.cfBlocked;
        d["response_body"] = diag.body;
    } else {
        rec.cloud_setting_id = newCloudId;
        rec.cloud_synced_at  = (uint32_t)time(nullptr);
        g_user_filaments.upsert(rec);
        resp["cloud_setting_id"] = newCloudId;
    }
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

// ── Printer CRUD + state ────────────────────────────────────

static void serializePrinter(JsonObject out, const PrinterConfig& cfg, const BambuPrinter* p) {
    JsonDocument doc;
    cfg.toJson(doc, /*include_secret*/ false);
    for (JsonPair kv : doc.as<JsonObject>()) out[kv.key()] = kv.value();

    JsonObject st = out["state"].to<JsonObject>();
    if (!p) {
        st["link"] = "disconnected";
        return;
    }
    const PrinterState& s = p->state();
    switch (s.link) {
        case BambuLinkState::Connected:    st["link"] = "connected"; break;
        case BambuLinkState::Connecting:   st["link"] = "connecting"; break;
        case BambuLinkState::Failed:       st["link"] = "failed"; break;
        default:                           st["link"] = "disconnected"; break;
    }
    st["gcode_state"]   = s.gcode_state;
    st["progress_pct"]  = s.progress_pct;
    st["layer_num"]     = s.layer_num;
    st["total_layers"]  = s.total_layers;
    st["bed_temp"]      = s.bed_temp;
    st["nozzle_temp"]   = s.nozzle_temp;
    st["active_tray"]   = s.active_tray;
    st["last_report_ms_ago"] =
        s.last_report_ms ? (int)(millis() - s.last_report_ms) : -1;
    if (s.error_message.length()) st["error"] = s.error_message;

    auto writeTray = [](JsonObject j, const AmsTray& tr) {
        j["id"]             = tr.id;
        j["material"]       = tr.tray_type;
        j["color"]          = tr.tray_color;
        j["tray_info_idx"]  = tr.tray_info_idx;
        j["tag_uid"]        = tr.tag_uid;
        j["spool_id"]       = tr.mapped_spool_id;
        j["spool_override"] = tr.mapped_via_override;
        j["remain_pct"]     = tr.remain_pct;
        j["nozzle_min_c"]   = tr.nozzle_min_c;
        j["nozzle_max_c"]   = tr.nozzle_max_c;
        j["k"]              = tr.k;
        j["cali_idx"]       = tr.cali_idx;
    };

    JsonArray ams = st["ams"].to<JsonArray>();
    for (int u = 0; u < s.ams_count; ++u) {
        JsonObject unit = ams.add<JsonObject>();
        unit["id"]       = s.ams[u].id;
        unit["humidity"] = s.ams[u].humidity;
        JsonArray trays = unit["trays"].to<JsonArray>();
        for (int t = 0; t < 4; ++t) {
            const AmsTray& tr = s.ams[u].trays[t];
            if (tr.id < 0) continue;
            writeTray(trays.add<JsonObject>(), tr);
        }
    }
    if (s.has_vt_tray) {
        // External spool holder — sibling of `ams`, not an extra AMS unit.
        writeTray(st["vt_tray"].to<JsonObject>(), s.vt_tray);
    }
}

void ConsoleWebServer::_handlePrintersList(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& cfg : g_printers_cfg.list()) {
        JsonObject row = arr.add<JsonObject>();
        serializePrinter(row, cfg, g_bambu.find(cfg.serial));
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handlePrinterGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String serial = req->pathArg(0);
    const PrinterConfig* cfg = g_printers_cfg.find(serial);
    if (!cfg) { req->send(404, "application/json", "{\"error\":\"not found\"}"); return; }
    JsonDocument doc;
    JsonObject row = doc.to<JsonObject>();
    serializePrinter(row, *cfg, g_bambu.find(serial));
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handlePrinterPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    PrinterConfig p;
    if (!p.fromJson(doc)) {
        req->send(400, "application/json", "{\"error\":\"serial + access_code required\"}");
        return;
    }
    if (g_printers_cfg.list().size() >= BAMBU_MAX_PRINTERS && !g_printers_cfg.find(p.serial)) {
        req->send(400, "application/json", "{\"error\":\"max printers reached\"}");
        return;
    }
    g_printers_cfg.upsert(p);
    g_bambu.reload();
    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handlePrinterDelete(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String serial = req->pathArg(0);
    if (!g_printers_cfg.remove(serial)) {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    g_bambu.reload();
    req->send(200, "application/json", "{\"ok\":true}");
}

// Kick off an FTP+gcode analysis for a printer. The fetch can take seconds so
// we dispatch it to a short-lived FreeRTOS task on core 0 (network core) and
// return 202 immediately. Progress is then read via GET /analysis. The task
// self-destructs once BambuPrinter::analyseRemote() returns.
struct AnalyzeTaskCtx {
    BambuPrinter* printer;
    String        path;
};

static void _analyzeTaskTrampoline(void* arg) {
    auto* ctx = static_cast<AnalyzeTaskCtx*>(arg);
    ctx->printer->analyseRemote(ctx->path);
    delete ctx;
    vTaskDelete(nullptr);
}

void ConsoleWebServer::_handlePrinterAnalyzeStart(AsyncWebServerRequest* req,
                                                  uint8_t* data, size_t len) {
    String serial = req->pathArg(0);
    BambuPrinter* p = g_bambu.find(serial);
    if (!p) { req->send(404, "application/json", "{\"error\":\"unknown printer\"}"); return; }
    if (p->analysisInProgress()) {
        req->send(409, "application/json", "{\"error\":\"analysis already running\"}");
        return;
    }
    String path = "/cache/.3mf";
    if (len > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, data, len)) {
            String requested = doc["path"] | "";
            if (requested.length()) path = requested;
        }
    }
    auto* ctx = new AnalyzeTaskCtx{p, path};
    // 16 KB stack — see matching note in bambu_printer.cpp where the
    // print-start auto-analyse task is spawned; mbedtls handshake + ZIP
    // parser + PrinterFtp locals tipped over 8 KiB in practice.
    BaseType_t rc = xTaskCreatePinnedToCore(_analyzeTaskTrampoline, "ana", 16384, ctx,
                                            1, nullptr, 0);
    if (rc != pdPASS) {
        delete ctx;
        req->send(500, "application/json", "{\"error\":\"task alloc failed\"}");
        return;
    }
    req->send(202, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handlePrinterAnalysisGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String serial = req->pathArg(0);
    BambuPrinter* p = g_bambu.find(serial);
    if (!p) { req->send(404, "application/json", "{\"error\":\"unknown printer\"}"); return; }
    const auto& a = p->lastAnalysis();
    const PrinterState& s = p->state();
    // Current progress clamped to [0..100]; drives the live `grams_consumed`
    // lookup. -1 (no report yet) collapses to 0 so the web UI can still
    // render "0.0 / X g" while we wait for the first MQTT tick.
    int pct = s.progress_pct;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    JsonDocument doc;
    doc["in_progress"]   = p->analysisInProgress();
    doc["valid"]         = a.valid;
    doc["path"]          = a.path;
    doc["error"]         = a.error;
    doc["started_ms"]    = a.started_ms;
    doc["finished_ms"]   = a.finished_ms;
    doc["total_grams"]   = a.total_grams;
    doc["total_mm"]      = a.total_mm;
    doc["has_pct_table"] = a.has_pct_table;
    doc["progress_pct"]  = pct;
    doc["gcode_state"]   = s.gcode_state;
    JsonArray tools = doc["tools"].to<JsonArray>();
    for (int i = 0; i < a.tool_count; ++i) {
        const auto& t = a.tools[i];
        JsonObject row = tools.add<JsonObject>();
        row["tool_idx"] = t.tool_idx;
        row["grams"]    = t.grams;
        row["mm"]       = t.mm;
        row["ams_unit"] = t.ams_unit;
        row["slot_id"]  = t.slot_id;
        row["spool_id"] = t.spool_id;
        row["material"] = t.material;
        row["color"]    = t.color;
        // Live consumption: exact from the M73 table when present, otherwise
        // linear. Mirrors the logic in BambuPrinter::_commitIncrementalConsumption.
        float consumed = 0.f;
        if (a.valid && t.tool_idx >= 0 && t.tool_idx < 16) {
            if (a.has_pct_table) {
                consumed = a.grams_at_pct[pct][t.tool_idx];
            } else {
                consumed = (float)pct / 100.f * t.grams;
            }
        }
        row["grams_consumed"] = consumed;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handlePrinterAmsMappingPost(AsyncWebServerRequest* req,
                                                    uint8_t* data, size_t len) {
    String serial = req->pathArg(0);
    PrinterConfig* cfg = const_cast<PrinterConfig*>(g_printers_cfg.find(serial));
    if (!cfg) {
        req->send(404, "application/json", "{\"error\":\"unknown printer\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    if (!doc["ams_unit"].is<int>() || !doc["slot_id"].is<int>()) {
        req->send(400, "application/json", "{\"error\":\"ams_unit and slot_id (int) required\"}");
        return;
    }
    int ams_unit = doc["ams_unit"];
    int slot_id  = doc["slot_id"];
    // spool_id: empty string (or missing / null) clears the override for
    // that slot; any other value sets it.
    String spool_id = doc["spool_id"] | "";

    // Validate that the spool exists when setting (empty = clear). This
    // keeps us from pinning a slot to a ghost id the user can't fix from
    // the UI.
    if (spool_id.length() && _store) {
        SpoolRecord rec;
        if (!_store->findByTagId(spool_id, rec) && !_store->findById(spool_id, rec)) {
            req->send(400, "application/json", "{\"error\":\"unknown spool id\"}");
            return;
        }
    }

    cfg->setAmsOverride(ams_unit, slot_id, spool_id);
    g_printers_cfg.save();

    // Nudge the live BambuPrinter (if any) so the next AMS report resolves
    // with the new override. The simplest way is to re-copy the config onto
    // the running instance via updateConfig().
    if (BambuPrinter* live = g_bambu.find(serial)) {
        live->updateConfig(*cfg);
    }

    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handlePrinterFtpDebug(AsyncWebServerRequest* req,
                                              uint8_t* data, size_t len) {
    String serial = req->pathArg(0);
    BambuPrinter* p = g_bambu.find(serial);
    if (!p) { req->send(404, "application/json", "{\"error\":\"unknown printer\"}"); return; }
    JsonDocument doc;
    if (len && deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String op   = doc["op"]   | "probe";
    String path = doc["path"] | "";
    if (op != "probe" && op != "list" && op != "download") {
        req->send(400, "application/json", "{\"error\":\"op must be probe|list|download\"}");
        return;
    }
    auto sink = std::make_shared<BambuPrinter::FtpStreamCtx>();
    if (!sink->sb) {
        req->send(503, "application/json", "{\"error\":\"stream buffer alloc failed\"}");
        return;
    }
    if (!p->startFtpDebug(op, path, sink)) {
        req->send(409, "application/json",
                  "{\"error\":\"FTP debug already running\"}");
        return;
    }
    // Chunked NDJSON response: the filler drains the FTP task's stream
    // buffer on the AsyncTCP thread. RESPONSE_TRY_AGAIN asks the server
    // to call us back later — gives us TCP backpressure without polling.
    auto* resp = req->beginChunkedResponse(
        "application/x-ndjson",
        [sink](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
            if (!sink->sb) return 0;
            size_t got = xStreamBufferReceive(sink->sb, buf, maxLen, 0);
            if (got > 0) return got;
            if (sink->done) return 0;   // FTP task finished + buffer empty → end
            return RESPONSE_TRY_AGAIN;
        }
    );
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    req->send(resp);
}

void ConsoleWebServer::_handleDiscoveryPrinters(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    uint32_t now = millis();
    for (const auto& e : g_bambu_discovery.entries()) {
        JsonObject row = arr.add<JsonObject>();
        row["serial"]        = e.serial;
        row["ip"]            = e.ip.toString();
        row["model"]         = e.model;
        row["last_seen_ago"] = (int)(now - e.last_seen_ms);
        row["configured"]    = g_printers_cfg.find(e.serial) != nullptr;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static const char* handshakeStr(ScaleLink::Handshake h) {
    switch (h) {
        case ScaleLink::Handshake::Encrypted:   return "encrypted";
        case ScaleLink::Handshake::Unencrypted: return "unencrypted";
        case ScaleLink::Handshake::Failed:      return "failed";
        default:                                return "disconnected";
    }
}

void ConsoleWebServer::_handleDiscoveryScales(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    uint32_t now = millis();
    String pairedIp    = _scale ? _scale->scaleIp()   : "";
    String pairedName  = _scale ? _scale->scaleName() : "";
    bool   pairedConn  = _scale ? _scale->isConnected() : false;
    const char* pairedHandshake = _scale ? handshakeStr(_scale->handshake()) : "disconnected";

    for (const auto& e : g_scale_discovery.entries()) {
        JsonObject row = arr.add<JsonObject>();
        row["name"]          = e.name;
        row["ip"]            = e.ip.toString();
        row["last_seen_ago"] = (int)(now - e.last_seen_ms);
        bool paired = (e.name == pairedName) || (e.ip.toString() == pairedIp);
        row["paired"]        = paired;
        row["connected"]     = paired && pairedConn;
        row["handshake"]     = paired ? pairedHandshake : "disconnected";
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleDisplayConfigGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    doc["sleep_timeout_s"] = (uint32_t)ConsoleDisplay::sleepTimeout();
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleDisplayConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    if (!doc["sleep_timeout_s"].is<uint32_t>()) {
        req->send(400, "application/json", "{\"error\":\"sleep_timeout_s required\"}");
        return;
    }
    uint32_t s = doc["sleep_timeout_s"].as<uint32_t>();
    // Clamp to a sensible upper bound — more than an hour of idle makes the
    // feature useless and would mask a forgotten "never sleep" setting.
    if (s > 3600) s = 3600;

    Preferences p;
    p.begin(NVS_NS_DISPLAY, false);
    p.putUInt(NVS_KEY_DISP_SLEEP_S, s);
    p.end();
    ConsoleDisplay::setSleepTimeout(s);   // applies immediately + wakes screen

    JsonDocument resp;
    resp["ok"] = true;
    resp["sleep_timeout_s"] = s;
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

// Per-scale secret API. The secret is keyed by scale name (what the scale
// announces over SSDP as its USN) so the console can pair with multiple
// scales, each with its own shared secret. The `ScaleSecrets` helper owns
// the NVS JSON map; the legacy `NVS_KEY_SCALE_SECRET` single-secret key is
// no longer consulted — it was the source of the "reboot wipes my secret"
// bug because these handlers wrote there while `_refreshHandshakeState()`
// read from the map instead.

void ConsoleWebServer::_handleScaleSecretGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    String name = req->hasParam("name") ? req->getParam("name")->value() : "";
    if (name.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"name query param required\"}");
        return;
    }
    JsonDocument doc;
    doc["name"]       = name;
    doc["configured"] = ScaleSecrets::configured(name);
    doc["preview"]    = ScaleSecrets::preview(name);
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleScaleSecretPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String name   = doc["name"]   | "";
    String secret = doc["secret"] | "";
    if (name.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"name required\"}");
        return;
    }
    // Empty secret clears the entry — same helper handles both cases.
    ScaleSecrets::set(name, secret);
    // Ping the link state so the UI's handshake indicator updates immediately
    // without waiting for a reconnect event.
    if (_scale) _scale->pokeHandshake();
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── Bambu Lab cloud auth ─────────────────────────────────────

// Map a step result onto an HTTP response. Same shape regardless of
// which step triggered it, so the React side has one parser to write.
//
// On any non-Ok outcome (including the multi-step branches) the response
// also carries a `diagnostics` block — request URL, HTTP status, raw
// response body, collected response headers — so the UI's collapsible
// "show details" panel can reveal exactly what came back. Skipped on
// Ok because the response body there contains the access token.
static void _sendBambuStepResult(AsyncWebServerRequest* req,
                                 BambuCloudAuth::StepResult res,
                                 BambuCloudAuth::Region region,
                                 const String& accountForSave = "") {
    using S = BambuCloudAuth::StepStatus;
    JsonDocument doc;
    int code = 200;
    switch (res.status) {
        case S::Ok:
            doc["status"]  = "ok";
            doc["message"] = res.message;
            // Persist on success — the frontend doesn't need to round-trip
            // the token through itself; if it ever does want to display it
            // a separate `show_token` flow is gated by auth.
            g_bambu_cloud.saveToken(res.token, region, accountForSave);
            break;
        case S::NeedEmailCode:
            doc["status"]  = "need_email_code";
            doc["message"] = res.message;
            break;
        case S::NeedTfa:
            doc["status"]  = "need_tfa";
            doc["tfa_key"] = res.tfaKey;
            doc["message"] = res.message;
            break;
        case S::InvalidCreds:
            code = 401;
            doc["status"]  = "invalid_credentials";
            doc["message"] = res.message;
            break;
        case S::NetworkError:
            code = 502;
            doc["status"]  = "network_error";
            doc["message"] = res.message;
            break;
        case S::ServerError:
        default:
            code = 502;
            doc["status"]  = "server_error";
            doc["message"] = res.message;
            break;
    }
    if (res.status != S::Ok) {
        JsonObject diag = doc["diagnostics"].to<JsonObject>();
        diag["request_url"]      = res.requestUrl;
        diag["http_status"]      = res.httpStatus;
        diag["response_headers"] = res.responseHeaders;
        diag["response_body"]    = res.responseBody;
    }
    String out; serializeJson(doc, out);
    req->send(code, "application/json", out);
}

void ConsoleWebServer::_handleBambuCloudGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    bool configured = g_bambu_cloud.haveToken();
    doc["configured"] = configured;
    doc["region"]     = BambuCloudAuth::regionToString(g_bambu_cloud.region());
    doc["email"]      = g_bambu_cloud.email();
    if (configured) {
        const String& tok = g_bambu_cloud.token();
        // Surface the token to the UI so the user can copy it. Auth on
        // this endpoint is the device's fixed key; if the device is in
        // open-key (Change-Me!) mode, the token is effectively
        // accessible to anyone on the LAN — same blast radius as the
        // OrcaSlicer token file the user would otherwise have on disk.
        doc["token"]      = tok;
        // Also pre-compute a short preview for the masked default view.
        doc["token_preview"] = tok.length() > 12
            ? tok.substring(0, 6) + "…" + tok.substring(tok.length() - 4)
            : tok;
        uint32_t exp = BambuCloudAuth::decodeExpiry(tok);
        doc["expires_at"] = exp;     // Unix timestamp; 0 = unknown
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleBambuCloudLogin(AsyncWebServerRequest* req,
                                              uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String account  = body["account"]  | "";
    String password = body["password"] | "";
    auto region = BambuCloudAuth::regionFromString(body["region"] | "global");
    if (account.isEmpty() || password.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"account+password required\"}");
        return;
    }
    auto res = g_bambu_cloud.loginPassword(account, password, region);
    _sendBambuStepResult(req, res, region, account);
}

void ConsoleWebServer::_handleBambuCloudLoginCode(AsyncWebServerRequest* req,
                                                  uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String account = body["account"] | "";
    String code    = body["code"]    | "";
    auto region = BambuCloudAuth::regionFromString(body["region"] | "global");
    if (account.isEmpty() || code.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"account+code required\"}");
        return;
    }
    auto res = g_bambu_cloud.loginEmailCode(account, code, region);
    _sendBambuStepResult(req, res, region, account);
}

void ConsoleWebServer::_handleBambuCloudLoginTfa(AsyncWebServerRequest* req,
                                                 uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String tfaKey  = body["tfa_key"]  | "";
    String tfaCode = body["tfa_code"] | "";
    String account = body["account"]  | "";
    auto region = BambuCloudAuth::regionFromString(body["region"] | "global");
    if (tfaKey.isEmpty() || tfaCode.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"tfa_key+tfa_code required\"}");
        return;
    }
    auto res = g_bambu_cloud.loginTfa(tfaKey, tfaCode, region);
    _sendBambuStepResult(req, res, region, account);
}

void ConsoleWebServer::_handleBambuCloudSetToken(AsyncWebServerRequest* req,
                                                 uint8_t* data, size_t len) {
    if (!_requireAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String pasted = body["token"] | "";
    String token, region_str, email;
    bool   blob = false;
    if (BambuCloudAuth::unpackTokenBlob(pasted, token, region_str, email)) {
        // SPOOLHARD-TOKEN: blob from tools/bambu_login.py — token /
        // region / email all come from the blob and override anything
        // the user typed in the per-field UI inputs.
        blob = true;
    } else if (pasted.startsWith("SPOOLHARD-TOKEN:")) {
        // Has the prefix but failed to decode → user pasted a corrupted
        // blob. Don't fall through to "raw JWT" treatment, that would
        // hide the real failure.
        req->send(400, "application/json",
                  "{\"error\":\"corrupted SPOOLHARD-TOKEN blob — "
                  "re-run tools/bambu_login.py and copy the whole line\"}");
        return;
    } else {
        token      = pasted;
        region_str = body["region"] | "global";
        email      = body["email"]  | "";
    }
    if (token.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"token required\"}");
        return;
    }
    auto region = BambuCloudAuth::regionFromString(region_str);

    // Soft verify. The verify endpoint sits behind the same Cloudflare
    // wall that blocks the login endpoint, so a perfectly valid token
    // can come back as Unreachable from a CF-blocked WAN. Save anyway
    // in that case + tell the UI so it can warn the user that cloud
    // features won't work until network/fingerprint changes.
    auto vr = g_bambu_cloud.verifyToken(token, region);
    using V = BambuCloudAuth::VerifyResult;
    JsonDocument resp;
    if (vr == V::Rejected) {
        resp["status"]  = "invalid_credentials";
        resp["message"] = "token rejected by /v1/user-service/my/profile";
        String out; serializeJson(resp, out);
        req->send(401, "application/json", out);
        return;
    }
    g_bambu_cloud.saveToken(token, region, email);
    resp["status"]   = "ok";
    resp["verified"] = (vr == V::Verified);
    resp["blob"]     = blob;
    if (vr == V::Unreachable) {
        resp["message"] = "saved, but couldn't reach Bambu Cloud to verify "
                          "(api.bambulab.com unreachable from this device); "
                          "cloud-dependent features may not work";
    }
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleBambuCloudVerify(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!g_bambu_cloud.haveToken()) {
        req->send(404, "application/json", "{\"error\":\"no token stored\"}");
        return;
    }
    auto vr = g_bambu_cloud.verifyToken(g_bambu_cloud.token(), g_bambu_cloud.region());
    using V = BambuCloudAuth::VerifyResult;
    JsonDocument doc;
    doc["valid"] = (vr == V::Verified);
    if (vr == V::Unreachable) {
        doc["unreachable"] = true;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleBambuCloudClear(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    g_bambu_cloud.clearToken();
    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handleScaleLinkStatus(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    if (_scale) {
        doc["connected"] = _scale->isConnected();
        doc["handshake"] = handshakeStr(_scale->handshake());
        doc["ip"]        = _scale->scaleIp();
        doc["name"]      = _scale->scaleName();

        JsonObject ev = doc["last_event"].to<JsonObject>();
        if (_scale->lastEventMs() > 0) {
            ev["kind"]   = _scale->lastEventKind();
            ev["detail"] = _scale->lastEventDetail();
            ev["scale_name"] = _scale->lastEventScale();
            ev["ago_ms"] = (int)(millis() - _scale->lastEventMs());
        } else {
            ev["kind"]   = "";
            ev["detail"] = "";
            ev["scale_name"] = "";
            ev["ago_ms"] = -1;
        }

        // Latest weight snapshot for "capture current weight" in the spool
        // editor. `state` is the stability signal; the UI will only accept
        // "stable" as a capture trigger to avoid committing a wobbly reading.
        // `precision` mirrors the scale's saved decimal-places setting so
        // the dashboard shows the same precision the scale itself uses
        // (with the raw float still available for a muted full-precision
        // fallback alongside).
        JsonObject w = doc["weight"].to<JsonObject>();
        w["precision"] = _scale->scalePrecision();
        if (_scale->lastWeightMs() > 0) {
            w["grams"]  = _scale->lastWeightG();
            w["state"]  = _scale->lastWeightState();
            w["ago_ms"] = (int)(millis() - _scale->lastWeightMs());
        } else {
            w["grams"]  = 0;
            w["state"]  = "";
            w["ago_ms"] = -1;
        }
    } else {
        doc["connected"] = false;
    }
    String r; serializeJson(doc, r);
    req->send(200, "application/json", r);
}

// ── Core weights DB ────────────────────────────────────────────
// Auto-populated by the on-device wizard; editable from the web UI.

void ConsoleWebServer::_handleCoreWeightsGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : CoreWeights::list()) {
        JsonObject o = arr.add<JsonObject>();
        o["brand"]      = e.brand;
        o["material"]   = e.material;
        o["advertised"] = e.advertised;
        o["grams"]      = e.grams;
        o["updated_ms"] = e.updated_ms;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleCoreWeightsPut(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    String brand    = doc["brand"]    | "";
    String material = doc["material"] | "";
    int advertised  = doc["advertised"] | 0;
    int grams       = doc["grams"]      | -1;
    if (brand.isEmpty() || material.isEmpty() || advertised <= 0 || grams < 0) {
        req->send(400, "application/json",
            "{\"error\":\"brand, material, advertised (>0) and grams (>=0) required\"}");
        return;
    }
    CoreWeights::set(brand, material, advertised, grams);
    req->send(200, "application/json", "{\"ok\":true}");
}

void ConsoleWebServer::_handleCoreWeightsDelete(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    if (!req->hasParam("key")) {
        req->send(400, "application/json", "{\"error\":\"key query param required\"}");
        return;
    }
    String key = req->getParam("key")->value();
    bool ok = CoreWeights::removeKey(key);
    req->send(ok ? 200 : 404, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"not found\"}");
}

// ── Quick-weights ──────────────────────────────────────────────

void ConsoleWebServer::_handleQuickWeightsGet(AsyncWebServerRequest* req) {
    if (!_requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc["grams"].to<JsonArray>();
    for (int g : QuickWeights::get()) arr.add(g);
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void ConsoleWebServer::_handleQuickWeightsPost(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    if (!doc["grams"].is<JsonArrayConst>()) {
        req->send(400, "application/json", "{\"error\":\"grams (array) required\"}");
        return;
    }
    std::vector<int> v;
    for (JsonVariantConst x : doc["grams"].as<JsonArrayConst>()) {
        int g = x | 0;
        if (g > 0) v.push_back(g);
    }
    QuickWeights::set(v);
    req->send(200, "application/json", "{\"ok\":true}");
}
