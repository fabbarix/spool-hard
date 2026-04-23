#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <memory>
#include "printer_config.h"

// Snapshot of one AMS tray slot (mirrors yanshay/SpoolEase PrintTray).
struct AmsTray {
    int     id          = -1;       // 0..3 within its AMS
    String  tray_type;              // e.g. "PLA"
    String  tray_color;             // "RRGGBBAA" hex
    String  tray_info_idx;          // Bambu material code e.g. "GFL99"
    String  tag_uid;                // RFID tag if Bambu-tagged spool
    int     nozzle_min_c = 0;
    int     nozzle_max_c = 0;
    int     remain_pct   = -1;      // -1 = unknown
    String  mapped_spool_id;        // resolved via SpoolStore::findByTagId
    bool    mapped_via_override = false;  // true when the mapping came from a
                                          // user-set PrinterConfig override
                                          // rather than an automatic tag_uid
                                          // lookup
    float   k            = 0.f;     // pressure-advance K (0 = unset)
    int     cali_idx     = -1;      // K calibration index reported by Bambu
};

struct AmsUnit {
    int     id = -1;                // 0..3 if multi-AMS
    int     humidity = -1;          // 1..5 (5=best)
    AmsTray trays[4];
};

enum class BambuLinkState { Disconnected, Connecting, Connected, Failed };

// Result of a completed FTP+gcode analysis run. Per-tool grams are keyed by
// the printer's tool index (0..15). `ams_unit` / `slot_id` and `spool_id` are
// backfilled from the AMS snapshot at the time the analysis completed so the
// frontend can correlate tools with physical trays and spools.
struct GcodeAnalysisTool {
    int    tool_idx   = -1;
    float  grams      = 0.f;
    float  mm         = 0.f;
    int    ams_unit   = -1;
    int    slot_id    = -1;
    String spool_id;
    String material;     // PLA/ABS/... from the tray info (for UI)
    String color;        // RRGGBBAA
};

struct GcodeAnalysis {
    bool   valid         = false;
    uint32_t started_ms  = 0;
    uint32_t finished_ms = 0;
    String path;
    String error;
    float  total_grams   = 0.f;
    float  total_mm      = 0.f;
    int    tool_count    = 0;
    GcodeAnalysisTool tools[16];

    // Progress-indexed grams lookup — [pct 0..100][tool_idx 0..15] — populated
    // from the slicer's `M73 P<n>` hints in the 3mf gcode. Valid only when
    // has_pct_table is true; otherwise live-consume falls back to linear
    // extrapolation from total_grams. 16 tools × 101 pcts × 4 B ≈ 6.4 KiB.
    bool   has_pct_table = false;
    float  grams_at_pct[101][16] = {};
};

struct PrinterState {
    BambuLinkState link = BambuLinkState::Disconnected;
    String  gcode_state;            // IDLE|PREPARE|RUNNING|PAUSE|FINISH|FAILED
    int     progress_pct = -1;
    int     layer_num    = -1;
    int     total_layers = -1;
    float   bed_temp     = 0.f;
    float   nozzle_temp  = 0.f;
    int     active_tray  = -1;      // tray_now
    AmsUnit ams[4];                 // up to 4 AMS units on H2D; only ams[0] on X1/P1
    int     ams_count    = 0;
    // External / "virtual" tray for single-extruder printers that feed from a
    // regular spool holder instead of an AMS. Bambu reports it at
    // `print.vt_tray` at the top level, separate from `print.ams.ams[]`.
    // `has_vt_tray` = false means this printer didn't report one in the last
    // pushall — we skip rendering it rather than show an empty card.
    AmsTray vt_tray;
    bool    has_vt_tray  = false;
    // Nozzle inventory from print.device.nozzle.info. X1/P1 typically expose
    // one nozzle (0.4 by default); H2D exposes two. Needed to key K-values
    // correctly since Bambu's pressure-advance is per-nozzle-diameter.
    float   nozzle_diameters[2] = {0.f, 0.f};
    int     nozzle_count = 0;
    uint32_t last_report_ms = 0;
    String  error_message;          // last connection/parse error, for dashboards
};

class WiFiClientSecure;
class PubSubClient;

/**
 * One Bambu printer connection: TLS MQTT on :8883. `loop()` must be called
 * repeatedly (it's non-blocking). Connection reattempts happen every 5 s.
 */
class BambuPrinter {
public:
    // Fired once per successful `PendingAms::claim(...)` — i.e. a user-scanned
    // spool has just been auto-assigned to a physical AMS slot on some
    // connected printer. Used by main.cpp to auto-dismiss the spool-detail
    // screen on the LCD once the spool is confirmed in-AMS. Singleton because
    // claim itself is singleton (only one armed spool at a time).
    using SpoolAssignedCb = std::function<void(const String& spool_id,
                                               const String& printer_name,
                                               int ams_unit, int slot_id)>;
    static void setOnSpoolAssigned(SpoolAssignedCb cb) { s_onSpoolAssigned = std::move(cb); }

    explicit BambuPrinter(const PrinterConfig& cfg);
    ~BambuPrinter();

    void loop();                          // call regularly (non-blocking)
    void requestFullStatus();             // pushes pushall on device/request

    const PrinterState& state() const     { return _state; }
    const PrinterConfig& config() const   { return _cfg; }
    const GcodeAnalysis& lastAnalysis() const { return _lastAnalysis; }
    bool  analysisInProgress() const      { return _analysisInProgress; }

    void updateConfig(const PrinterConfig& cfg);

    // Fetch `path` from the printer over FTPS, stream-extract the first .gcode
    // entry from the 3MF, and run it through GCodeAnalyzer. Blocks the caller.
    // Default path targets the current print job. Returns false on transport
    // or parse error (details in _lastAnalysis.error).
    bool analyseRemote(const String& path = "/cache/.3mf");

    // Interactive FTP debug — kicks off a background task that runs the
    // requested operation against the printer's FTPS server and pushes
    // NDJSON events (one `{"kind":"trace"...}` or `{"kind":"done"...}` JSON
    // object per line) into `sink`. The web handler wraps `sink` in a
    // chunked HTTP response, so the browser receives events progressively
    // with natural TCP backpressure — no per-client message-queue limits
    // like the old AsyncWebSocket broadcast had. `op` is one of "probe"
    // / "list" / "download". For "download", the file is streamed to
    // SD:/ftp_dl.bin and served via GET /api/ftp-download.
    //
    // Ownership: `sink` is a shared_ptr so the HTTP filler callback and
    // the FTP task can both hold references — whoever drops last frees
    // the stream buffer.
    struct FtpStreamCtx {
        StreamBufferHandle_t sb  = nullptr;
        volatile bool        done = false;
        FtpStreamCtx();
        ~FtpStreamCtx();
        void emit(const JsonDocument& doc);  // serializes doc + '\n' into sb
    };
    bool startFtpDebug(const String& op, const String& path,
                       std::shared_ptr<FtpStreamCtx> sink);
    bool ftpDebugBusy() const { return _ftpDebugBusy; }
    // Implementation — public only so the FreeRTOS task trampoline (a free
    // function in bambu_printer.cpp) can call it.
    void _runFtpDebug(const String& op, const String& path,
                      std::shared_ptr<FtpStreamCtx> sink);

private:
    PrinterConfig      _cfg;
    PrinterState       _state;
    WiFiClientSecure*  _wifi    = nullptr;
    PubSubClient*      _mqtt    = nullptr;
    uint32_t           _lastConnectAttemptMs = 0;
    uint32_t           _lastPollMs           = 0;

    // Background connect machinery. PubSubClient::connect() is synchronous
    // and against an unreachable printer takes ~30 s (TLS handshake on top
    // of a TCP connect that lwIP retries 12× with backoff). Doing that on
    // the main loop starves every other subsystem (scale WS, NFC, LVGL,
    // web, …). We hand it to a one-shot FreeRTOS task pinned to core 0
    // and the main loop just polls the result. While Pending, no other
    // _mqtt method is touched from loop()-side code — PubSubClient is
    // not thread-safe, so the task gets exclusive access.
    enum class ConnectTaskState : uint8_t { Idle, Pending, Done };
    volatile ConnectTaskState _connectTaskState  = ConnectTaskState::Idle;
    volatile bool             _connectTaskResult = false;
    TaskHandle_t              _connectTask       = nullptr;
    static void               _connectTrampoline(void* arg);

    // Tracks the last tag_uid seen in each [ams_unit][slot] so we can detect
    // edge-triggered "spool loaded" events and auto-restore K-values once
    // per insertion instead of on every report.
    String _lastTagUid[4][4];

    // Signature of each slot's (tray_type | tray_info_idx | tray_color)
    // tuple from the previous AMS report, used to detect empty → populated
    // transitions for the PendingAms auto-assignment feature. Separate from
    // _lastTagUid because most SpoolHard spools are NOT Bambu-RFID — we need
    // a detector that fires when the user sets the filament type on the
    // printer's own touch panel after inserting a tagless spool.
    String _lastSlotSignature[4][4];
    bool   _slotSignatureSeen[4][4] = {{false}};

    // Last spool_id we pushed ams_filament_setting for, per slot. When the
    // resolved mapping changes to a new non-empty spool (e.g. the user set a
    // manual AMS override on the web, or a PendingAms auto-assign landed),
    // we push the SpoolRecord's color/material/temps so the printer's panel
    // matches our definition. Tracked separately from the transition
    // signature above because a mapping can change without any slot
    // occupancy change.
    String _lastSyncedSpoolId[4][4];
    String _lastSyncedSpoolIdVt;   // vt_tray (external) sibling

    // External-tray signature tracker — mirror of _lastSlotSignature for
    // the vt_tray. Used to detect physical-unload on the external spool
    // holder before resolution so the sticky override can be dropped.
    String _lastVtSignature;
    bool   _vtSignatureSeen = false;

    GcodeAnalysis _lastAnalysis;
    volatile bool _analysisInProgress = false;

    // Previous gcode_state, retained so we can detect edge transitions:
    //   * IDLE/PREPARE → RUNNING  → kick off analyseRemote so we have per-
    //     tool gram totals ready to commit when the print ends.
    //   * RUNNING/PAUSE → FINISH/IDLE/FAILED → commit those totals into the
    //     mapped spools' consumed_since_add / consumed_since_weight.
    // Guarded so each transition only fires once per print.
    String _prevGcodeState;
    bool   _analysisCommitted = false;  // cleared on RUNNING start

    // Retry gate for analyseRemote: the TLS handshake against the printer's
    // FTPS server needs a ~30 KiB contiguous internal-DRAM block for the
    // mbedtls buffers, which is often not available right at print-start
    // (MQTT/Web/LVGL have just churned through a lot of heap). When a
    // previous attempt failed we back off a few seconds and retry from
    // loop(), up to a generous cap, so normally-transient memory pressure
    // doesn't leave the print without a 3mf analysis for its whole run.
    uint32_t _analysisNextRetryAt = 0;   // millis; 0 = nothing scheduled
    uint8_t  _analysisAttempts    = 0;   // count per-print (reset on RUNNING)

    // How much of the current print's predicted per-tool grams has already
    // been pushed into each mapped spool's consumed_since_weight. Reset to 0
    // on RUNNING entry; bumped in 5% steps by _commitIncrementalConsumption
    // as `print.mc_percent` advances. The terminal commit applies the
    // remaining (100 - _progressCommittedPct) fraction so the total still
    // matches the analysis' predicted grams regardless of how finely we
    // stepped during the print.
    int    _progressCommittedPct = 0;

    static SpoolAssignedCb s_onSpoolAssigned;

    void _ensureConnected();
    void _onMessage(const char* topic, uint8_t* payload, unsigned int length);
    void _parseReport(const JsonDocument& doc);
    void _parseAms(const JsonObjectConst& amsObj, PrinterState& out);
    void _persistKValues();
    // Sync the AMS-reported tray_info_idx onto every mapped spool whose
    // current `slicer_filament` doesn't match. Bambu re-keys this when
    // the user picks a filament from the printer's panel after a tag
    // load — without this we'd never learn the printer's chosen idx.
    void _persistTrayInfoIdx();
    void _autoRestoreKValues();
    void _pushKRestore(int ams_id, int slot_id, const AmsTray& tr, float nozzle, float k);
    // If PendingAms has an armed spool, claim it and push ams_filament_setting
    // to this printer for (ams_id, slot_id). Called from _parseAms after an
    // empty/populated transition on the slot signature. Also sets a manual
    // AMS override so subsequent print-consumption accounting maps the slot
    // to the right spool without needing a second NFC/RFID read.
    void _maybeAssignPendingSpool(int ams_unit, int slot_id);
    void _pushAmsFilamentSetting(int ams_unit, int slot_id, const class SpoolRecord& rec);
    // If the resolved spool mapping for this slot changed to a new non-empty
    // id since the last tick, push the spool's definition (color / material /
    // temps / slicer_filament) to the printer so its panel reflects reality.
    // Edge-triggered via _lastSyncedSpoolId*; skips unmount events so a
    // spool-pull doesn't wipe the printer's settings.
    void _syncSpoolToPrinter(int ams_unit, int slot_id, const class AmsTray& tr);
    void _handleGcodeStateTransition(const String& prev, const String& now);
    void _commitPrintConsumption();
    // Called once per BambuPrinter::loop() tick to spawn a retry of
    // analyseRemote when the previous attempt hit a memory-pressure
    // failure. Needs to live alongside the task trampoline at the bottom
    // of bambu_printer.cpp.
    void _maybeRetryAnalysis();

    volatile bool _ftpDebugBusy = false;
    // Live consumption — called on every MQTT report update while in an
    // active gcode_state. Pushes the delta between _progressCommittedPct
    // and the current `mc_percent` (quantised to 5% steps to cap flash
    // wear) into each mapped spool's consumed_since_weight.
    void _commitIncrementalConsumption();
};
