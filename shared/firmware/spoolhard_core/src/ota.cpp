#include "spoolhard/ota.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <time.h>

// NVS schema — canonical names. Both products use the same namespace
// + keys, so the shared lib hardcodes them rather than parameterising.
// If you ever need a per-product override, plumb it through OtaConfig.
namespace {
constexpr const char* kNvsNs              = "ota_cfg";
constexpr const char* kKeyUrl             = "url";
constexpr const char* kKeyUseSsl          = "use_ssl";
constexpr const char* kKeyVerify          = "verify_ssl";
constexpr const char* kKeyCheckEnabled    = "ck_en";
constexpr const char* kKeyCheckIntervalH  = "ck_hrs";
constexpr const char* kKeyLastTs          = "lck_ts";
constexpr const char* kKeyLastStatus      = "lck_st";
constexpr const char* kKeyLastFw          = "lk_fw";
constexpr const char* kKeyLastFe          = "lk_fe";
}

OtaChecker g_ota_checker;
OtaInFlight g_ota_in_flight;

// ── OtaConfig persistence ────────────────────────────────────

void OtaConfig::load() {
    Preferences prefs;
    prefs.begin(kNvsNs, true);
    url               = prefs.getString(kKeyUrl,    OTA_DEFAULT_URL);
    use_ssl           = prefs.getBool(kKeyUseSsl,  true);
    verify_ssl        = prefs.getBool(kKeyVerify,   true);
    check_enabled     = prefs.getBool(kKeyCheckEnabled, true);
    check_interval_h  = prefs.getUInt(kKeyCheckIntervalH, 24);
    last_check_ts     = prefs.getUInt(kKeyLastTs,  0);
    last_check_status = prefs.getString(kKeyLastStatus, "");
    last_known_fw     = prefs.getString(kKeyLastFw, "");
    last_known_fe     = prefs.getString(kKeyLastFe, "");
    prefs.end();
    if (check_interval_h < kOtaCheckIntervalMin) check_interval_h = kOtaCheckIntervalMin;
    if (check_interval_h > kOtaCheckIntervalMax) check_interval_h = kOtaCheckIntervalMax;
}

void OtaConfig::save() const {
    Preferences prefs;
    prefs.begin(kNvsNs, false);
    prefs.putString(kKeyUrl,        url);
    prefs.putBool(kKeyUseSsl,      use_ssl);
    prefs.putBool(kKeyVerify,       verify_ssl);
    prefs.putBool(kKeyCheckEnabled,     check_enabled);
    prefs.putUInt(kKeyCheckIntervalH,    check_interval_h);
    prefs.putUInt(kKeyLastTs,      last_check_ts);
    prefs.putString(kKeyLastStatus, last_check_status);
    prefs.putString(kKeyLastFw,    last_known_fw);
    prefs.putString(kKeyLastFe,    last_known_fe);
    prefs.end();
}

// ── Strict-semver "is latest > current" ──────────────────────
//
// Parse "X.Y.Z[-suffix]" — split on the first '-'. Numeric core is
// compared lexicographically over its three uints. If the cores tie,
// pre-release ordering applies: anything with a suffix is *less* than
// the bare version (per semver §11.4), so "0.3.0-alpha < 0.3.0".
// Two suffixes compare as plain strings — good enough for our
// "alpha-N" / "beta-N" pattern.
//
// Anything we can't parse is treated as "not newer" — defensive: we
// don't want a malformed manifest to spam upgrade prompts.
struct _Sem {
    uint32_t major = 0, minor = 0, patch = 0;
    String   suffix;
    bool     ok = false;
};
static _Sem _parseSemver(const String& v) {
    _Sem s;
    if (v.isEmpty()) return s;
    int dash = v.indexOf('-');
    String core = (dash >= 0) ? v.substring(0, dash) : v;
    s.suffix = (dash >= 0) ? v.substring(dash + 1) : String();
    int dot1 = core.indexOf('.');
    if (dot1 < 0) return s;
    int dot2 = core.indexOf('.', dot1 + 1);
    if (dot2 < 0) return s;
    s.major = core.substring(0, dot1).toInt();
    s.minor = core.substring(dot1 + 1, dot2).toInt();
    s.patch = core.substring(dot2 + 1).toInt();
    s.ok = true;
    return s;
}
bool otaVersionNewerThan(const String& latest, const String& current) {
    _Sem a = _parseSemver(latest);
    _Sem b = _parseSemver(current);
    if (!a.ok || !b.ok) return false;
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    if (a.patch != b.patch) return a.patch > b.patch;
    // Cores equal — pre-release rules.
    if (a.suffix.isEmpty() &&  b.suffix.isEmpty()) return false;
    if (a.suffix.isEmpty() && !b.suffix.isEmpty()) return true;     // 0.3.0  > 0.3.0-alpha
    if (!a.suffix.isEmpty() && b.suffix.isEmpty()) return false;    // 0.3.0-alpha < 0.3.0
    return a.suffix > b.suffix;
}

// ── Manifest fetch ────────────────────────────────────────────

OtaManifest otaFetchManifest(const OtaConfig& cfg, String* statusOut) {
    OtaManifest m;
    auto setStatus = [&](const char* s) { if (statusOut) *statusOut = s; };

    if (cfg.url.isEmpty()) { setStatus("network"); return m; }

    String body;
    int code = 0;
    if (cfg.use_ssl) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(20000);
        http.setReuse(false);
        if (!http.begin(client, cfg.url)) { setStatus("network"); return m; }
        // arduino-esp32's HTTPClient follows 302s out of the box for
        // GitHub release URLs which redirect to the asset CDN.
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        code = http.GET();
        if (code > 0) body = http.getString();
        http.end();
    } else {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(20000);
        http.setReuse(false);
        if (!http.begin(client, cfg.url)) { setStatus("network"); return m; }
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        code = http.GET();
        if (code > 0) body = http.getString();
        http.end();
    }

    if (code <= 0)              { setStatus("network");    return m; }
    if (code < 200 || code > 299) { setStatus("http_error"); return m; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) { setStatus("parse_error"); return m; }

    if (!doc["firmware"].is<JsonObject>()) { setStatus("parse_error"); return m; }
    m.firmware_version = doc["firmware"]["version"] | "";
    m.firmware_url     = doc["firmware"]["url"]     | "";
    m.firmware_size    = doc["firmware"]["size"]    | 0;
    m.firmware_sha256  = doc["firmware"]["sha256"]  | "";
    if (m.firmware_version.isEmpty() || m.firmware_url.isEmpty()) {
        setStatus("parse_error"); return m;
    }
    if (doc["frontend"].is<JsonObject>()) {
        m.has_frontend      = true;
        m.frontend_version  = doc["frontend"]["version"] | "";
        m.frontend_url      = doc["frontend"]["url"]     | "";
        m.frontend_size     = doc["frontend"]["size"]    | 0;
        m.frontend_sha256   = doc["frontend"]["sha256"]  | "";
        // If the block exists but is broken, drop frontend silently
        // rather than failing the whole check.
        if (m.frontend_version.isEmpty() || m.frontend_url.isEmpty()) {
            m.has_frontend = false;
        }
    }
    m.valid = true;
    setStatus("ok");
    return m;
}

// ── Periodic checker (main-loop poll + one-shot task per check) ──

void OtaChecker::begin() {
    OtaConfig cfg; cfg.load();
    _lastCheckTs   = cfg.last_check_ts;
    _lastStatus    = cfg.last_check_status;
    if (cfg.last_known_fw.length()) {
        _lastManifest.firmware_version = cfg.last_known_fw;
    }
    if (cfg.last_known_fe.length()) {
        _lastManifest.frontend_version = cfg.last_known_fe;
        _lastManifest.has_frontend = true;
    }
    // _lastManifest.valid stays false — we only consult cached
    // version strings for display, not for triggering updates.
    _bootMs = millis();
    Serial.printf("[OTA] checker enabled=%d interval=%uh last=%u (%s)\n",
                  cfg.check_enabled, cfg.check_interval_h,
                  cfg.last_check_ts, cfg.last_check_status.c_str());
}

void OtaChecker::update() {
    if (_checkInFlight) return;
    if (WiFi.status() != WL_CONNECTED) return;

    OtaConfig cfg; cfg.load();
    if (!cfg.check_enabled && !_forceNext) return;

    uint32_t now = millis();
    bool firstAfterBoot = (_lastCheckSessionMs == 0) &&
                          (now - _bootMs > 60UL * 1000UL);
    bool dueByInterval  = _lastCheckSessionMs > 0 &&
                          (now - _lastCheckSessionMs >
                           cfg.check_interval_h * 60UL * 60UL * 1000UL);
    if (!_forceNext && !firstAfterBoot && !dueByInterval) return;

    _forceNext = false;
    _lastCheckSessionMs = now;
    _checkInFlight = true;
    Serial.println("[OTA] check starting");
    // Pinned to core 0 so the LVGL task on core 1 stays smooth during
    // the HTTPS round-trip. 8 KB stack is enough for HTTPClient +
    // mbedtls handshake; we don't decode big payloads here.
    xTaskCreatePinnedToCore(_runCheckTask, "ota_check", 8192,
                            this, 1, nullptr, 0);
}

void OtaChecker::_runCheckTask(void* arg) {
    static_cast<OtaChecker*>(arg)->_runCheck();
    vTaskDelete(nullptr);
}

void OtaChecker::_runCheck() {
    OtaConfig cfg; cfg.load();
    String status;
    OtaManifest m = otaFetchManifest(cfg, &status);

    _lastStatus   = status;
    if (m.valid) _lastManifest = m;
    // Wall-clock timestamp via SNTP if available (configured at boot).
    // Falls back to 0 if SNTP hasn't synced yet — the UI shows that
    // gracefully.
    time_t t = 0;
    time(&t);
    if (t > 1704067200) {  // 2024-01-01 sanity check
        _lastCheckTs = (uint32_t)t;
    }

    cfg.last_check_ts     = _lastCheckTs;
    cfg.last_check_status = _lastStatus;
    if (m.valid) {
        cfg.last_known_fw = m.firmware_version;
        cfg.last_known_fe = m.has_frontend ? m.frontend_version : String();
    }
    cfg.save();

    if (m.valid) {
        Serial.printf("[OTA] check ok: fw=%s fe=%s\n",
                      m.firmware_version.c_str(),
                      m.has_frontend ? m.frontend_version.c_str() : "(none)");
    } else {
        Serial.printf("[OTA] check failed: %s\n", _lastStatus.c_str());
    }
    _checkInFlight = false;
}

OtaPending OtaChecker::pending() const {
    OtaPending p;
    p.firmware_current = FW_VERSION;
    p.frontend_current = FE_VERSION;
    if (!_lastManifest.firmware_version.isEmpty()) {
        p.firmware_latest = _lastManifest.firmware_version;
        p.firmware = otaVersionNewerThan(_lastManifest.firmware_version, FW_VERSION);
    }
    if (_lastManifest.has_frontend && !_lastManifest.frontend_version.isEmpty()) {
        p.frontend_latest = _lastManifest.frontend_version;
        p.frontend = otaVersionNewerThan(_lastManifest.frontend_version, FE_VERSION);
    }
    return p;
}

// ── Streaming download → Update.write with sha256 verify ─────

namespace {
String _sha256Hex(const uint8_t* digest) {
    static const char* hex = "0123456789abcdef";
    String s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i) {
        s += hex[(digest[i] >> 4) & 0xf];
        s += hex[(digest[i]) & 0xf];
    }
    return s;
}

// Pull `url` from the network and pump the body through Update.write()
// while accumulating sha256. Returns true iff the bytes came in clean,
// the announced size matches, and the sha256 matches (when provided).
// `partition` is U_FLASH (firmware) or U_SPIFFS (frontend).
bool _streamToUpdate(const String& url, uint32_t expectedSize,
                     const String& expectedSha256, int partition,
                     bool useSsl,
                     std::function<void(int)> percentCb) {
    // arduino-esp32's HTTPClient::begin overloads take WiFiClient& or
    // WiFiClientSecure& concretely (no abstract Client&), so we have
    // to branch the begin() call rather than pass a base pointer.
    HTTPClient http;
    http.setTimeout(30000);
    http.setReuse(false);

    WiFiClientSecure secure;
    WiFiClient       plain;
    bool ok;
    if (useSsl) {
        secure.setInsecure();
        ok = http.begin(secure, url);
    } else {
        ok = http.begin(plain, url);
    }
    if (!ok) {
        Serial.printf("[OTA] http.begin failed for %s\n", url.c_str());
        return false;
    }
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] GET %d for %s\n", code, url.c_str());
        http.end();
        return false;
    }
    int total = http.getSize();
    if (total <= 0) total = expectedSize;
    if (total <= 0) {
        Serial.println("[OTA] unknown content size — refusing");
        http.end();
        return false;
    }

    if (!Update.begin(total, partition)) {
        Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        http.end();
        return false;
    }

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    WiFiClient* stream = http.getStreamPtr();
    static const size_t kBuf = 4096;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kBuf]);
    int written = 0;
    int lastPct = -1;
    uint32_t lastYield = millis();
    while (http.connected() && written < total) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastYield > 30000) {
                Serial.println("[OTA] stream stalled — aborting");
                Update.abort();
                mbedtls_sha256_free(&shaCtx);
                http.end();
                return false;
            }
            delay(5);
            continue;
        }
        size_t n = stream->readBytes(buf.get(), avail > kBuf ? kBuf : avail);
        if (n == 0) continue;
        if (Update.write(buf.get(), n) != n) {
            Serial.printf("[OTA] Update.write short: %s\n", Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&shaCtx);
            http.end();
            return false;
        }
        mbedtls_sha256_update(&shaCtx, buf.get(), n);
        written += n;
        lastYield = millis();
        if (percentCb) {
            int pct = (int)((int64_t)written * 100 / total);
            if (pct != lastPct) {
                percentCb(pct);
                lastPct = pct;
            }
        }
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&shaCtx, digest);
    mbedtls_sha256_free(&shaCtx);

    if (written != total) {
        Serial.printf("[OTA] short read: got %d expected %d\n", written, total);
        Update.abort();
        http.end();
        return false;
    }
    if (!expectedSha256.isEmpty()) {
        String got = _sha256Hex(digest);
        if (!got.equalsIgnoreCase(expectedSha256)) {
            Serial.printf("[OTA] sha256 mismatch: got %s expected %s\n",
                          got.c_str(), expectedSha256.c_str());
            Update.abort();
            http.end();
            return false;
        }
    }
    if (!Update.end(true)) {
        Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        http.end();
        return false;
    }
    http.end();
    return true;
}
}  // namespace

bool otaRun(const OtaConfig& cfg, std::function<void(OtaProgress)> cb) {
    Serial.printf("[OTA] otaRun starting from %s\n", cfg.url.c_str());

    String status;
    OtaManifest m = otaFetchManifest(cfg, &status);
    if (!m.valid) {
        Serial.printf("[OTA] manifest fetch failed: %s\n", status.c_str());
        return false;
    }

    bool fwNeeded = otaVersionNewerThan(m.firmware_version, FW_VERSION);
    bool feNeeded = m.has_frontend &&
                    otaVersionNewerThan(m.frontend_version, FE_VERSION);
    if (!fwNeeded && !feNeeded) {
        Serial.println("[OTA] already up to date");
        if (cb) cb({OtaProgress::Kind::Done, 100});
        return false;
    }

    bool anyApplied = false;

    if (fwNeeded) {
        Serial.printf("[OTA] firmware: %s → %s\n", FW_VERSION, m.firmware_version.c_str());
        if (cb) cb({OtaProgress::Kind::Firmware, 0});
        bool ok = _streamToUpdate(m.firmware_url, m.firmware_size,
                                  m.firmware_sha256, U_FLASH, cfg.use_ssl,
                                  [&](int p){ if (cb) cb({OtaProgress::Kind::Firmware, p}); });
        if (!ok) {
            Serial.println("[OTA] firmware update aborted");
            return false;
        }
        anyApplied = true;
    }
    if (feNeeded) {
        Serial.printf("[OTA] frontend: %s → %s\n", FE_VERSION, m.frontend_version.c_str());
        if (cb) cb({OtaProgress::Kind::Frontend, 0});
        bool ok = _streamToUpdate(m.frontend_url, m.frontend_size,
                                  m.frontend_sha256, U_SPIFFS, cfg.use_ssl,
                                  [&](int p){ if (cb) cb({OtaProgress::Kind::Frontend, p}); });
        if (!ok) {
            // Firmware already flashed in the previous step — boot it
            // anyway. The user can re-trigger the frontend update once
            // the new firmware is up.
            Serial.println("[OTA] frontend update failed, but firmware was applied — rebooting");
        } else {
            anyApplied = true;
        }
    }

    if (cb) cb({OtaProgress::Kind::Done, 100});
    if (anyApplied) {
        Serial.println("[OTA] reboot in 1s");
        delay(1000);
        ESP.restart();
        return true;
    }
    return false;
}

bool otaRun(const OtaConfig& cfg, std::function<void(int)> percentCb) {
    return otaRun(cfg, [percentCb](OtaProgress p) {
        if (percentCb && p.kind != OtaProgress::Kind::Idle &&
                         p.kind != OtaProgress::Kind::Done) {
            percentCb(p.percent);
        }
    });
}
