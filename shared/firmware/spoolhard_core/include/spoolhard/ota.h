#pragma once
#include <Arduino.h>
#include <functional>

// Per-product compile-time identity. These come in as -D flags from
// the consumer's platformio.ini build_flags so the shared lib can be
// built standalone (no product-specific config.h on the include path).
#ifndef OTA_DEFAULT_URL
#error "OTA_DEFAULT_URL must be set in the consumer's platformio.ini build_flags"
#endif
#ifndef FW_VERSION
#error "FW_VERSION must be set (typically by patch_version.py)"
#endif
#ifndef FE_VERSION
#error "FE_VERSION must be set (typically by patch_version.py)"
#endif

// OTA config — everything persisted in NVS namespace `ota_cfg`.
//
// `url` now points at a *manifest* (JSON) rather than a raw binary.
// The manifest carries entries for both the firmware app and the
// SPIFFS frontend image plus their sha256s, so a single check tells
// us what needs updating. Older single-binary URLs continue to work
// in a degraded mode — see manifest-parser fallback in ota.cpp.
//
// Scheduled version-check fields: `check_enabled`, `check_interval_h`.
// Telemetry (written by the checker, surfaced in the UI):
// `last_check_ts`, `last_check_status`, `last_known_fw`, `last_known_fe`.
struct OtaConfig {
    String   url                = OTA_DEFAULT_URL;
    bool     use_ssl            = true;
    bool     verify_ssl         = true;
    bool     check_enabled      = true;
    uint32_t check_interval_h   = 24;  // allowed values: 1 / 6 / 24 / 168
    uint32_t last_check_ts      = 0;   // Unix epoch, 0 = never checked
    String   last_check_status;        // "ok" | "network" | "http_error" | "parse_error" | ""
    String   last_known_fw;            // e.g. "0.3.0"; empty if never seen
    String   last_known_fe;

    void load();
    void save() const;
};

// Minimum acceptable check interval. 1 h is tight but gives a useful
// "set this to 1h while I'm iterating" ceiling during dev.
constexpr uint32_t kOtaCheckIntervalMin = 1;
constexpr uint32_t kOtaCheckIntervalMax = 168;  // 7d

// One-shot manifest fetch + parse + compare. Shape mirrors release.sh's
// manifest.json: {firmware:{version,url,size,sha256}, frontend:{…}}.
// Either block may be absent — frontend is treated as optional.
struct OtaManifest {
    bool   valid = false;
    String firmware_version;
    String firmware_url;
    uint32_t firmware_size = 0;
    String firmware_sha256;
    bool   has_frontend = false;
    String frontend_version;
    String frontend_url;
    uint32_t frontend_size = 0;
    String frontend_sha256;
};

// Result flags for the "what's pending on this device" query.
struct OtaPending {
    bool   firmware = false;
    bool   frontend = false;
    String firmware_current;
    String firmware_latest;
    String frontend_current;
    String frontend_latest;
};

// Strict semver ("X.Y.Z[-suffix]") newer-than comparator. Handles the
// common cases: numeric X.Y.Z comparison, and pre-release suffix
// ("0.3.0-alpha-1" < "0.3.0"). Used to decide whether to prompt the
// user on a fetched manifest. Returns true iff `latest` is strictly
// greater than `current`; equal and lesser both return false.
bool otaVersionNewerThan(const String& latest, const String& current);

// One-shot manifest fetch. Returns parsed manifest; `valid == false`
// on any transport, HTTP or parse failure. `statusOut` is filled with
// a short tag ("ok" / "network" / "http_error" / "parse_error") for
// surfacing in the UI. Blocking — caller should run this on a task.
OtaManifest otaFetchManifest(const OtaConfig& cfg, String* statusOut = nullptr);

// The scheduler that drives periodic checks. Lives on the main loop:
// cheap millis() comparison per tick; fires a one-shot FreeRTOS task
// on its own to do the actual HTTPS fetch (doesn't block main).
class OtaChecker {
public:
    void begin();
    void update();                           // call each loop iteration

    // Trigger a check on the next tick regardless of schedule.
    void kickNow() { _forceNext = true; }

    // Last known state. Protected against partial reads during a task
    // update because all assignments are 32-bit or String (implicit
    // atomic-enough on ESP32; we don't need full thread safety for
    // display-only data).
    OtaManifest   lastManifest() const { return _lastManifest; }
    OtaPending    pending()      const;
    uint32_t      lastCheckTs()  const { return _lastCheckTs; }
    const String& lastStatus()   const { return _lastStatus; }
    bool          checkInFlight() const { return _checkInFlight; }

private:
    uint32_t      _lastCheckSessionMs = 0;   // millis() reset on boot
    uint32_t      _lastCheckTs        = 0;   // Unix epoch, persisted
    String        _lastStatus;
    OtaManifest   _lastManifest;
    volatile bool _checkInFlight      = false;
    volatile bool _forceNext          = false;
    uint32_t      _bootMs             = 0;

    static void _runCheckTask(void* arg);
    void        _runCheck();
};

extern OtaChecker g_ota_checker;

// Manifest-driven update runner. Fetches the manifest, then for each
// product block whose version differs from the running version:
// downloads the bin, verifies size + sha256, flashes to the right
// partition, and (after BOTH are done, if both changed) reboots.
//
// Sequential: firmware first, then frontend. If firmware succeeds but
// frontend fails, we still reboot into the new firmware — the user
// can retry the frontend afterwards. If firmware fails, we abort
// before touching anything else.
//
// `progressCb(int percent)` fires as the firmware then frontend
// streams in; the UI interprets `kind` to know which of the two is
// running. Returns true only if at least one component was actually
// flashed and the device is about to reboot.
struct OtaProgress {
    enum class Kind { Idle, Firmware, Frontend, Done };
    Kind kind = Kind::Idle;
    int  percent = 0;
};

bool otaRun(const OtaConfig& cfg,
            std::function<void(OtaProgress)> progressCb = nullptr);

// Back-compat overload for the LCD + web callers that pass a
// percent-only callback. Same semantics; Kind is inferred internally.
bool otaRun(const OtaConfig& cfg, std::function<void(int)> progressCb);
