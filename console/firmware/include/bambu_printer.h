#pragma once
#include <atomic>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <esp_random.h>
#include <freertos/stream_buffer.h>
#include <memory>
#include <vector>
#include "printer_config.h"
#include "spool_record.h"

// Snapshot of one AMS tray slot (mirrors yanshay/SpoolEase PrintTray).
struct AmsTray {
    int     id          = -1;       // 0..3 within its AMS
    String  tray_type;              // e.g. "PLA"
    String  tray_sub_brands;        // Bambu sub-brand string (e.g. "Y2"); "" when not reported
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

// Copy the AMS-reported fields on `tr` onto `rec`, touching only fields
// that differ from the current record. Used by both the web "import
// from printer" endpoint and the LCD slot-detail Import button so they
// stay in lockstep on which fields are adopted and how. Returns the
// names of SpoolRecord fields that actually changed; caller is
// responsible for persisting via SpoolStore::upsert when non-empty.
std::vector<String> applyAmsTrayToSpool(const AmsTray& tr, SpoolRecord& rec);

struct AmsUnit {
    int     id = -1;                // 0..3 if multi-AMS
    int     humidity = -1;          // 1..5 scaled (Bambu)
    int     humidity_raw = -1;      // raw % RH; -1 when not reported
    float   temp_c = -1000.f;       // chamber temp in °C; -1000 = not reported
    // Drying status (mirrors `dry_setting`). All sentinels mean "inactive";
    // serializeState only emits the drying block when duration_h > 0.
    // Bambu reports duration in hours (matches the per-tray `drying_time`
    // recommendation, e.g. ABS = 8h @ 80°C).
    int     dry_duration_h    = -1;
    int     dry_temperature_c = -1;
    String  dry_filament;
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

    // Live counters updated from the analyzer task while in_progress=true,
    // so the dashboard can show a per-second progress bar instead of a
    // mystery spinner during the 30-40 s body fetch+parse. Single writer
    // (analyzer task) + multiple readers (web /analysis poll); 32-bit
    // aligned reads on ESP32-S3 are atomic so we don't need a mutex.
    //   * progress_bytes:       bytes consumed by analyzer so far (post-
    //     decompression for deflate, raw for stored / un-zipped paths).
    //   * progress_total_bytes: expected total once known (set after
    //     EOCD parse for ZIP, after SIZE for raw). 0 = unknown, frontend
    //     should fall back to indeterminate.
    //   * running_grams / _mm:  accumulated totals as the parser walks
    //     extrusions. Same units as the final total_grams / total_mm.
    volatile uint32_t progress_bytes        = 0;
    volatile uint32_t progress_total_bytes  = 0;
    volatile float    running_grams         = 0.f;
    volatile float    running_mm            = 0.f;

    // In-place reset. `*this = GcodeAnalysis{}` materialises a ~6.5 KiB
    // temporary on the caller's stack (grams_at_pct alone is 6.4 KiB),
    // which overflows the Arduino loopTask's 8 KiB stack when the first
    // MQTT pushall after a fresh Bambu connect drives a gcode_state
    // transition through _handleGcodeStateTransition — bootlooped the
    // console as soon as the printer turned on. Zero in place instead.
    void reset();
};

// Timing/placement record of the last analysis run (inflate-placement
// measurement). Plain counters; single writer (analyse task), read by
// GET /api/debug/analysis. Always populated (a few clock reads per run).
struct InflateStats {
    bool     valid        = false;   // a run has completed (ok or failed)
    bool     ok           = false;
    bool     deflate      = false;   // false = stored/raw path (no inflate)
    bool     psram        = false;   // decompressor placement actually used
    bool     requested_psram = true; // flag value at run start
    bool     fallback     = false;   // wanted internal, alloc failed -> PSRAM
    uint32_t total_ms     = 0;       // whole analyseRemote wall time
    uint32_t stream_ms    = 0;       // FTP retrieve + inflate + feed
    uint32_t inflate_ms   = 0;       // time inside tinfl_decompress only
    uint32_t feed_ms      = 0;       // time inside analyzer.feed (deflate path)
    uint32_t bytes_in     = 0;       // compressed bytes consumed
    uint32_t bytes_out    = 0;       // inflated bytes produced
    uint32_t decomp_size  = 0;       // sizeof(tinfl_decompressor)
    uint32_t free_int_start = 0;     // free internal DRAM at run start
    uint32_t free_int_min   = 0;     // lowest free internal seen at chunk callbacks
};

// Session-only runtime switch (POST /api/debug/config {"inflate_psram":..}).
// true (default, = shipped behaviour) -> tinfl_decompressor in PSRAM;
// false -> internal DRAM (falls back to PSRAM if the alloc fails). The
// 32 KiB dict is always PSRAM. Not persisted; resets at boot.
extern std::atomic<bool> g_analysisInflatePsram;

struct PrinterState {
    BambuLinkState link = BambuLinkState::Disconnected;
    String  gcode_state;            // IDLE|PREPARE|RUNNING|PAUSE|FINISH|FAILED
    // Bambu's pushall reports `print.subtask_name` as the human-readable job
    // name. The 3MF on the printer's FTPS server lives at /cache/<name>.3mf
    // (sometimes the field already includes the `.3mf` suffix — strip
    // before re-appending). Used by analyseRemote() to build the FTP path
    // instead of guessing. Empty until the first MQTT report.
    String  subtask_name;
    // Bambu also publishes `print.gcode_file` — content is firmware-dependent:
    //   - X1 LAN print: ramdisk path like "/data/Metadata/plate_1.gcode"
    //     (NOT FTP-reachable; we filter these out).
    //   - P1 LAN print: bare filename "<name>.gcode.3mf" (this IS the
    //     on-disk filename and the path resolver tries it as a fallback).
    //   - Cloud / MakerWorld: usually empty, subtask_name carries the name.
    String  gcode_file;
    // Per-printer monotonic counter for prints. Bambu issues a fresh value
    // on every print start (e.g. "1948", "1949", …) and the same value
    // sticks for the duration of the job — across MQTT reconnects, console
    // reboots, and re-analyses. Empty / "0" on some firmware variants
    // (older P1, certain LAN-only first-prints) — `_jobKey()` falls back
    // to gcode_file in that case. Used by the consumption tracker as a
    // stable per-job identifier so re-running the analyzer doesn't
    // double-bill grams that were already committed.
    String  task_id;
    int     progress_pct = -1;
    int     layer_num    = -1;
    int     total_layers = -1;
    float   bed_temp     = 0.f;
    float   nozzle_temp  = 0.f;
    int     active_tray  = -1;      // tray_now
    AmsUnit ams[4];                 // up to 4 AMS units on H2D; only ams[0] on X1/P1
    int     ams_count    = 0;
    // External / "virtual" tray for single-extruder printers that feed from a
    // regular spool holder instead of an AMS. Older firmware (X1/P1/A1)
    // reports it at `print.vt_tray`; H2-series (H2S/H2D) reports a
    // `print.vir_slot[]` array instead, with ids 254 (deputy) / 255 (main).
    // `has_vt_tray` = false means this printer didn't report one in the last
    // pushall — we skip rendering it rather than show an empty card.
    //
    // Internally the external tray is always keyed as ams_unit 254 / slot 0
    // (overrides, active_tray, analysis tools) and `vt_tray.id` is 254.
    // `vt_wire_id` keeps the id the printer actually uses (254 for legacy
    // vt_tray, 254/255 for vir_slot) so outbound commands address it right.
    AmsTray vt_tray;
    bool    has_vt_tray  = false;
    int     vt_wire_id   = 254;
    bool    vt_is_vir_slot = false;
    // Nozzle inventory from print.device.nozzle.info. X1/P1 typically expose
    // one nozzle (0.4 by default); H2D exposes two. Needed to key K-values
    // correctly since Bambu's pressure-advance is per-nozzle-diameter.
    float   nozzle_diameters[2] = {0.f, 0.f};
    int     nozzle_count = 0;
    // Canonical short model code derived from the printer's serial
    // prefix (X1C / X1 / P1S / P1P / A1 / A1mini / H2D / H2S, …). Used
    // to resolve per-(printer,nozzle) variants on user filaments at
    // spool-load time. "" until the first MQTT report parses.
    String  model_code;
    uint32_t last_report_ms = 0;
    String  error_message;          // last connection/parse error, for dashboards
    // Ack of the last control command we published (print.command + result
    // in the printer's report). The H2S refuses unsigned LAN control commands
    // unless LAN Only + Developer Mode is on ("mqtt message verify failed",
    // err_code 84033543) -- publish() returns ok regardless, so this is the
    // only way to know a push actually landed.
    String   last_cmd;
    String   last_cmd_result;       // "success" | "failed" | ...
    String   last_cmd_reason;
    int32_t  last_cmd_err      = 0;
    uint32_t last_cmd_ms       = 0; // millis() of the ack, 0 = none seen
    bool     commands_rejected_auth = false;  // cleared by a later successful ack
    // Whether the last control command we published carried a signed
    // `header` (BambuSigner). A verify failure on a signed command means
    // the certificate was refused, not that Developer Mode is off.
    bool     last_cmd_signed   = false;
    // Signing certificate registration on this printer (app_cert_install /
    // app_cert_list; printers forget it on power cycle): 0 n/a (signing
    // off), 1 checking, 2 installing, 3 trusted, 4 failed.
    uint8_t  cert_trust = 0;
    String   cert_detail;           // reason on failure
    // Developer Mode (LAN control authorised): 0 unknown, 1 off, 2 on.
    // Derived from command acks (see _recordCommandAck) and, once
    // kFunBitDevMode is known, passively from print.fun. Seeded from the
    // persisted PrinterConfig::dev_mode.
    uint8_t  dev_mode = 0;
};

// arduino-esp32 3.x: WiFiClientSecure is a typedef of NetworkClientSecure,
// so forward-declare the real class and alias it.
class NetworkClientSecure;
using WiFiClientSecure = NetworkClientSecure;
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

    // Drop the current MQTT session (if any) and reset the 5 s reconnect
    // gate so the next loop() tick attempts a fresh TLS+MQTT connect
    // immediately. Used by the Refresh affordances (web button + LCD
    // button) and by the discovery callback when a configured printer's
    // IP has just changed under us. Safe while a background connect task
    // is in flight — the disconnect is skipped in that case and the new
    // connect will be picked up on the next harvest tick.
    void forceReconnect();

    const PrinterState& state() const     { return _state; }
    const PrinterConfig& config() const   { return _cfg; }
    const GcodeAnalysis& lastAnalysis() const { return _lastAnalysis; }
    bool  analysisInProgress() const      { return _analysisInProgress; }
    const InflateStats& inflateStats() const { return _inflateStats; }

    void updateConfig(const PrinterConfig& cfg);

    // Queue an explicit PendingAms claim for (ams_unit, slot_id) -- used by the
    // web "assign scanned spool" action. Thread-safe (called from AsyncTCP);
    // loop() consumes it on loopTask and runs _maybeAssignPendingSpool, i.e.
    // the identical path the empty->populated auto-claim uses. Returns false
    // if a previous request is still queued.
    bool requestClaimPending(int ams_unit, int slot_id);

    // Validate + queue an explicit claim: the single code path shared by the
    // web (POST /api/printers/<serial>/claim-pending) and the LCD
    // "choose destination" screen. `ams_unit`/`slot_id` are normalised in
    // place (external -> 254/0). On Queued, `spool_id_out` (if non-null) gets
    // the pending spool id. Thread-safe, never blocks, never publishes MQTT.
    enum class ClaimResult { Queued, BadSlot, NotConnected, NoSuchTray,
                             NothingPending, Busy };
    ClaimResult claimPendingExplicit(int& ams_unit, int& slot_id,
                                     String* spool_id_out = nullptr);

    // Forget what we last pushed per slot so the next report re-runs the
    // (edge-triggered) spool->printer sync. Thread-safe; applied in loop().
    // Used by the "Retry sync" action after the user enabled Developer Mode.
    void requestResync() { _resyncReq.store(true); }

    // "Test with printer" for command signing: re-sends one loaded slot's
    // CURRENT filament setting (as last reported, so nothing changes) as a
    // signed ams_filament_setting and records the printer's ack for it.
    // requestSignTest() is thread-safe (AsyncTCP); the publish happens in
    // loop() on loopTask. signTest() returns a copy, safe from any task.
    enum class SignTestState : uint8_t { Idle, Queued, Registering, Sent, Accepted,
                                         Refused, Failed, Timeout };
    struct SignTestResult {
        SignTestState state = SignTestState::Idle;
        int32_t  err        = 0;
        char     reason[96] = "";   // printer reason, or why it could not run
        char     slot[24]   = "";   // e.g. "AMS 1 slot 2", "External"
        uint32_t elapsed_ms = 0;    // send -> ack (or -> now while Sent)
    };
    bool requestSignTest();         // false while a test is queued/in flight
    SignTestResult signTest() const;

    // Fetch `path` from the printer over FTPS, stream-extract the first .gcode
    // entry from the 3MF, and run it through GCodeAnalyzer. Blocks the caller.
    // Default path targets the current print job. Returns false on transport
    // or parse error (details in _lastAnalysis.error).
    // Empty `path` (the default) means "auto-resolve from the current
    // MQTT subtask_name with an FTP-listing fallback". Pass an explicit
    // path only from the web FTP-debug flow, where the user is poking a
    // specific file.
    bool analyseRemote(const String& path = "");

    // Non-blocking variant of analyseRemote — spawns a FreeRTOS task
    // that calls analyseRemote(path) and exits. Tries the internal
    // heap first; falls back to a PSRAM-backed static stack if
    // contiguous internal DRAM isn't available (typical mid-print
    // when MQTT pushall + mbedtls + LVGL crowd the budget). Returns
    // false only if both internal and PSRAM allocations fail — in
    // that case the caller can retry later. Used by the web
    // POST /analyze handler and the auto-trigger on RUNNING edge.
    bool startAnalyseTask(const String& path);

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
    // Consecutive connect failures since last success. Drives an
    // exponential backoff on the retry interval so a printer that's
    // been off for hours doesn't keep churning lwIP / mbedtls heap
    // on a 5 s cycle (the alloc/free pattern that fragments internal
    // DRAM and panics the firmware overnight). Reset to 0 on success
    // OR on an SSDP rediscovery (printer just came back online).
    uint16_t           _connectFailureCount = 0;

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
    // Packed (ams_unit << 8 | slot_id) explicit claim request, -1 = none.
    std::atomic<int> _claimReq{-1};
    std::atomic<bool> _resyncReq{false};
    // Sign test (see requestSignTest). _signTest is guarded by _signTestMux;
    // _signTestSeq/_signTestStartMs are loopTask-only.
    std::atomic<bool> _signTestReq{false};
    mutable portMUX_TYPE _signTestMux = portMUX_INITIALIZER_UNLOCKED;
    SignTestResult _signTest;
    String   _signTestSeq;
    uint32_t _signTestStartMs = 0;
    bool     _signTestAwaitTrust = false;   // test waits for cert registration
    void _runSignTest();
    void _sendSignTest();
    // Cert registration state machine (loopTask). See cert_trust.
    enum class CertPhase : uint8_t { Idle, Listing, Polling };
    CertPhase _certPhase = CertPhase::Idle;
    String    _certSeq;               // sequence_id of the outstanding list query
    uint32_t  _certDeadline = 0;      // millis: reply timeout / next poll
    uint8_t   _certPolls    = 0;
    bool      _certInstalled = false; // app_cert_install sent this round
    uint32_t  _certLastStart = 0;
    uint32_t  _signerGen     = 0;
    bool      _resyncAfterTrust = false;  // a signed push was refused: re-push once trusted
    void _certStart(const char* why);
    void _certLoop();
    void _certOnList(JsonObjectConst sec);
    void _certQueryList();
    // Developer Mode probe: an unsigned command with a random name. The
    // printer checks signatures before command lookup, so 84033543 means
    // signing is required (Developer Mode off) and success means it is not.
    String    _devProbeSeq;
    void _sendDevModeProbe();
    void _setSignTest(SignTestState st, const char* reason, int32_t err = 0);
    // MQTT sequence_id source, kept in Bambu Studio's 20000-29999 window:
    // the H2S refuses SIGNED commands below 10000 (84033545), ids above
    // 2^31-1 hang the AMS (open-bamboo-networking research/06.02-mqtt.md),
    // and some firmware is reported to reject ids reused across a client
    // restart -- so the counter starts at a random point each boot and wraps
    // inside the window. (The H2S tested accepted reused ids.)
    std::atomic<uint32_t> _seq{esp_random() % 10000};
    String _nextSeq() { return String((unsigned long)(20000 + (_seq.fetch_add(1) + 1) % 10000)); }
    void _recordCommandAck(JsonObjectConst print);
    void _onSecurityReport(JsonObjectConst sec);
    // Update dev_mode; persists to PrinterConfig/NVS only when it changes.
    void _setDevMode(uint8_t v, const char* why);

    // Set once the printer has reported `device.extruder.info[].snow`
    // (H2-series). From then on that's the authoritative active-slot
    // source and `ams.tray_now` (stuck at "0" with no AMS) is ignored.
    bool   _haveExtruderSnow = false;
    // Wire id (254/255) of the external slot snow last said was feeding —
    // picks the right vir_slot[] entry on a dual-external H2D.
    int    _activeExtWireId  = 255;

    GcodeAnalysis _lastAnalysis;
    InflateStats  _inflateStats;
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
    void _parseExternalTray(const JsonObjectConst& vt, int wire_id, bool vir_slot);
    void _persistKValues();
    // Sync the AMS-reported tray_info_idx onto every mapped spool whose
    // current `slicer_filament` doesn't match. Bambu re-keys this when
    // the user picks a filament from the printer's panel after a tag
    // load — without this we'd never learn the printer's chosen idx.
    void _persistTrayInfoIdx();
    void _autoRestoreKValues();
    void _pushKRestore(int ams_id, int slot_id, const AmsTray& tr, float nozzle, float k);
    // Publish {"print": ...} to /request, signed when BambuSigner is enabled.
    // Records last_cmd_signed. Returns the PubSubClient result; `len` gets
    // the payload size for logging.
    bool _publishPrint(const JsonDocument& doc, size_t* len = nullptr);
    // If PendingAms has an armed spool, claim it and push ams_filament_setting
    // to this printer for (ams_id, slot_id). Called from _parseAms after an
    // empty/populated transition on the slot signature. Also sets a manual
    // AMS override so subsequent print-consumption accounting maps the slot
    // to the right spool without needing a second NFC/RFID read.
    bool _maybeAssignPendingSpool(int ams_unit, int slot_id);
    void _pushAmsFilamentSetting(int ams_unit, int slot_id, const class SpoolRecord& rec);
    // If the resolved spool mapping for this slot changed to a new non-empty
    // id since the last tick, push the spool's definition (color / material /
    // temps / slicer_filament) to the printer so its panel reflects reality.
    // Edge-triggered via _lastSyncedSpoolId*; skips unmount events so a
    // spool-pull doesn't wipe the printer's settings.
    void _syncSpoolToPrinter(int ams_unit, int slot_id, const class AmsTray& tr);
    void _handleGcodeStateTransition(const String& prev, const String& now);
    // Idempotent commit helper — see implementation. Both
    // _commitPrintConsumption (terminal, to_pct=100) and
    // _commitIncrementalConsumption (live mid-print) route through it.
    void _commitConsumptionTo(int to_pct, const String& job_key,
                              const char* reason_tag);
    // Stable identifier for the current print, used as the high-water-
    // mark key on each SpoolRecord. Prefers task_id ("1948" — Bambu's
    // own monotonic counter, stable across console reboots and
    // re-analyses); falls back to gcode_file when task_id is empty or
    // "0" (older firmwares, edge cases). Empty when no active print.
    String _jobKey() const;
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
