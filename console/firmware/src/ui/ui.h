#pragma once
#include <stdint.h>
#include <stddef.h>   // size_t — used by the calibration-wizard preset API

// Public API for the on-device LVGL UI. All functions are safe to call from
// any FreeRTOS task — they grab lv_lock() internally.

void ui_init();

void ui_show_splash();
void ui_show_onboarding();
void ui_show_home();

// Reusable full-screen "Are you sure?" confirm. Captures whatever screen
// is active when called and restores it on Cancel. Confirm runs `onConfirm`,
// which owns its own post-confirm navigation (reboot, OTA progress screen,
// …); if it returns without navigating away, the captured screen is
// restored. Used by the device-config screen for restart + apply-update.
typedef void (*ui_confirm_cb_t)(void);
void ui_show_confirm(const char* title, const char* body,
                     const char* confirmLabel, ui_confirm_cb_t onConfirm);
// Drive the on-device "Updating ..." screen. `title` is the bold heading
// (e.g. "Updating Firmware" / "Updating Frontend"), `version` the smaller
// label below it (filename until the version marker is parsed, then
// "v0.1.0.alpha-N"). Either may be nullptr to leave the existing text.
void ui_show_ota_progress(int percent, const char* title, const char* version);

// Update home-screen widgets. Called from Core-0 tasks (scale_link, nfc_reader).
// Legacy binary setter — kept for callers that don't care about handshake
// nuance. Prefer ui_set_scale_state() which shows the scale name and
// distinguishes "offline" / "no key" / "bad key" / "online".
void ui_set_scale_connected(bool connected);

// The LED colour on the home-screen scale card encodes the link state;
// the label next to it carries just the scale name (or "No scale" when
// nothing is paired). Four colours:
//   red    — missing       : no scale discovered / not paired yet
//   orange — discovering   : paired, but WS not currently connected
//   yellow — unencrypted   : WS up, no shared secret stored
//   green  — encrypted     : WS up + secret stored (handshake ok)
typedef enum {
    SCALE_LCD_MISSING      = 0,
    SCALE_LCD_DISCOVERING  = 1,
    SCALE_LCD_UNENCRYPTED  = 2,
    SCALE_LCD_ENCRYPTED    = 3,
} scale_lcd_state_t;

void ui_set_scale_state(scale_lcd_state_t state, const char* scale_name);

// Render the live weight on the home-screen weight card. `precision` is the
// scale's configured decimal-place setting (0..4), forwarded via the
// scale-link after the ScaleVersion handshake; we always render the headline
// number with that many decimals so the LCD and the scale's own screen
// agree. 0 is a sane default if no handshake has happened yet.
void ui_set_weight(float grams, const char* state, int precision = 0);
// Kept as a no-op for backward compat with main.cpp. The home screen used
// to have a "Last tag" card; its space was reclaimed for printer status
// and a larger AMS strip. Callers don't need to change anything.
void ui_set_last_tag(const char* uid, const char* url);
// Home-screen footer strip: hostname (left) / version (centred, static) /
// IP (right). Both setters are safe to call before/after wifi is up.
void ui_set_ip(const char* ip);

// WiFi status drives the colour of the icon next to the hostname on the
// home screen: green when connected, yellow while reconnecting, orange
// when the provisioning AP is live (no uplink), muted grey otherwise.
typedef enum {
    WIFI_LCD_DISCONNECTED = 0,
    WIFI_LCD_CONNECTING   = 1,
    WIFI_LCD_AP           = 2,   // provisioning SoftAP up, no uplink
    WIFI_LCD_CONNECTED    = 3,
} wifi_lcd_state_t;

void ui_set_hostname(const char* hostname, wifi_lcd_state_t state);

// Printer status card — shown just below the weight/scale row. Populated by
// main.cpp's periodic refresh from the first connected Bambu printer. Pass
// `connected=false` (or a null `snap`) to render the "no printer connected"
// placeholder.
struct UiPrinterSnapshot {
    bool    connected;
    char    name[24];
    char    gcode_state[16];   // "IDLE" | "PREPARE" | "RUNNING" | "PAUSE" | "FINISH" | "FAILED"
    int     progress_pct;      // 0..100, -1 if unknown
    int     layer_num;         // -1 if unknown
    int     total_layers;      // -1 if unknown
    float   nozzle_temp;       // °C
    float   bed_temp;          // °C
    bool    has_temps;         // false hides the temps line
};
void ui_set_printer_panel(const UiPrinterSnapshot* snap);

// Tap callback for the small refresh button on the printer card. main.cpp
// hooks this to fire `g_bambu_discovery.probe()` + `g_bambu.reconnectAll()`
// so the user can re-probe the LAN and force-reconnect without going to
// the web UI. Pass nullptr to disable; the button still renders, just
// becomes a no-op.
typedef void (*ui_printer_refresh_cb_t)(void);
void ui_set_printer_refresh_callback(ui_printer_refresh_cb_t cb);

// Update onboarding screen — called once from setup() with the values the
// WifiProvisioning module exposes.
void ui_set_onboarding(const char* ap_ssid, const char* security_key, const char* ip_or_mdns);

// ── OTA "update available" banner ──────────────────────────────
// Non-modal strip across the bottom of the home screen, hidden by default.
// `text` is the message to display ("Console v0.2.5 update available — tap to install");
// `show=false` hides the banner. Safe to call from any task.
void ui_set_ota_banner(bool show, const char* text);
// Wires the tap action — main.cpp registers a callback that kicks off the
// actual OTA flow. Tap is a no-op if no callback is set.
typedef void (*ui_ota_tap_cb_t)(void);
void ui_set_ota_banner_callback(ui_ota_tap_cb_t cb);

// ── Spool detail / weigh screen ─────────────────────────────
// Auto-opened when a tag is scanned locally on the console's PN532.
// Users can capture the scale's current weight as either the spool's
// `weight_current` (normal weigh-in) or `weight_core` (empty spool).
// The screen forwards button taps to a registered callback; main.cpp owns
// the actual store mutation so this file stays pure-UI.
typedef enum {
    SPOOL_BTN_CAPTURE_CURRENT = 1,
    SPOOL_BTN_CAPTURE_EMPTY   = 2,
    SPOOL_BTN_CLOSE           = 3,
} spool_btn_t;
typedef void (*spool_cb_t)(spool_btn_t action, const char* spool_id);

void ui_set_spool_callback(spool_cb_t cb);

// Populate and show the spool screen. `spool_id` is forwarded verbatim to
// the button callback. Pass -1 for any weight field that isn't known yet
// (it'll render as "--"). `color_hex` should be 6 or 8 hex chars without
// the leading `#`; pass "" for "no color".
void ui_show_spool(const char* spool_id,
                   const char* title,       // e.g. "Bambu PLA"
                   const char* subtitle,    // e.g. "Silver · tag AB:CD…"
                   const char* color_hex,
                   int  weight_current,
                   int  weight_advertised,
                   int  weight_core,
                   int  weight_new,
                   int  consumed_since_weight);

// Drive the live scale-weight readout on the spool screen. Called from
// scale_link's onWeight callback while the screen is visible — other
// screens ignore it.
void ui_set_spool_live_weight(float grams, const char* state);

// Drive the AMS auto-assignment status line on the spool screen. Null or
// empty text hides the line. Called from main.cpp when a tag scan arms the
// PendingAms latch, and from BambuPrinter when it pushes the pending spool
// metadata to a printer's AMS slot.
void ui_set_spool_ams_status(const char* text);

// Briefly pulse the spool-screen weight_current label so the user sees it
// was just updated — invoked from main.cpp after a successful scale-button
// capture. Three quick white↔brand toggles; no-op when the spool screen
// hasn't been built or isn't currently visible.
void ui_flash_spool_current();

// ── Weight-update confirmation screen ───────────────────────────
// Modal-style screen shown when the user taps "Capture weight" on the
// spool detail. Renders both the existing filament weight and the
// newly-weighed filament weight (both net of the spool's empty core)
// so the user can sanity-check the delta before committing. main.cpp
// computes the net values and passes them in; the user picks Accept
// (persist + flash) or Cancel (revert).
//
// If `new_g` is negative, the screen renders a "no empty-spool weight"
// warning instead of a number, and only Cancel is enabled — the user
// must capture an empty spool first to get a meaningful filament-only
// reading.
typedef enum {
    WEIGHT_CONFIRM_ACCEPT = 1,
    WEIGHT_CONFIRM_CANCEL = 2,
} weight_confirm_action_t;
typedef void (*weight_confirm_cb_t)(weight_confirm_action_t action);

// Register the callback once at boot (matches ui_set_spool_callback).
void ui_set_weight_confirm_callback(weight_confirm_cb_t cb);

// Switch to the confirm screen with the supplied numbers. Pass -1 for
// new_g to render the "no empty-spool weight known" warning instead of
// a number; the Accept button is disabled in that case.
void ui_show_weight_confirm(const char* spool_title,
                            int existing_g,
                            int new_g);

// ── Home-screen AMS panel ──────────────────────────────────────
// One-line snapshot of the currently selected printer's AMS trays + external
// spool holder. main.cpp builds this array from the first connected Bambu
// printer every ~1 s and pushes it in via ui_set_ams_panel(). Passing a null
// `status` string hides the panel entirely (e.g. no printer connected).
struct UiAmsSlot {
    char     label[8];       // short slot tag, e.g. "0·0", "Ext"
    char     material[12];   // tray_type — "PLA", "PETG", "" for empty
    uint32_t color_rgb;      // 0xRRGGBB (no alpha); 0 means "no colour known"
    int32_t  weight_g;       // from mapped SpoolRecord; -1 if not known
    float    k;              // pressure-advance K; 0 if unset
    int16_t  remain_pct;     // printer-reported remain %; -1 if unknown
    bool     active;         // is this the currently selected tray
    bool     occupied;       // false => render as empty placeholder
};

// `status` is a short top-line string like "P1S · RUNNING 42%" rendered
// above the tiles, or null/empty to hide the whole panel. `slots` is the
// tile array; slots with occupied=false and material="" render as empties.
// Exactly 5 entries are expected (AMS unit 0 tray 0..3 + external).
void ui_set_ams_panel(const char* status, const UiAmsSlot* slots);

// ── Slot detail screen ─────────────────────────────────────────
// Opens when the user taps one of the five AMS tiles. Shows everything
// the console knows about that slot: printer-reported (material, colour,
// K, remain%, tag_uid, tray_info_idx) plus the mapped SpoolRecord's full
// field set (brand, weights, temps, density, note, etc.). Closes via an
// on-screen button — returns to the home screen.
struct UiSlotDetail {
    // Header
    char     printer_name[24];
    char     slot_label[8];           // "AMS 0.0", "Ext"
    bool     active;

    // AMS-side (printer-reported) state — present even when no SpoolRecord
    // is mapped to the slot. All counts use -1 / 0 for "unknown".
    char     material[16];            // tray_type
    uint32_t color_rgb;               // 0 if unknown
    int16_t  remain_pct;              // -1 unset
    float    k;                       // 0 if unset
    int16_t  cali_idx;                // -1 unset
    bool     mapped_via_override;
    int16_t  ams_nozzle_min_c;
    int16_t  ams_nozzle_max_c;
    char     ams_tray_info_idx[16];
    char     tag_uid[40];

    // SpoolRecord-side — only populated when has_spool is true.
    bool     has_spool;
    char     spool_id[40];
    char     spool_tag_id[32];
    char     brand[24];
    char     material_subtype[16];
    char     color_name[24];
    char     color_code[10];          // "RRGGBB"
    int32_t  weight_current_g;
    int32_t  weight_advertised_g;
    int32_t  weight_core_g;
    int32_t  weight_new_g;
    float    consumed_since_weight_g;
    int32_t  nozzle_temp_min;
    int32_t  nozzle_temp_max;
    float    density;
    char     slicer_filament[16];
    char     note[64];
};

// Called when the user taps a home-screen AMS tile. slot_idx is 0..3 for
// AMS unit 0 trays and 4 for the external (vt_tray). main.cpp resolves
// the current state + mapped SpoolRecord and calls ui_show_slot_detail().
typedef void (*ui_slot_tap_cb_t)(int slot_idx);
void ui_set_slot_tap_callback(ui_slot_tap_cb_t cb);

// Show the detail screen with the packed info. Internally switches screen.
void ui_show_slot_detail(const UiSlotDetail* detail);

// User tapped the slot-detail "Import from printer" button. main.cpp
// resolves the currently-shown slot (the same slot_idx it last passed
// to ui_show_slot_detail) and invokes the firmware-side import helper.
typedef void (*ui_slot_import_cb_t)(void);
void ui_set_slot_import_callback(ui_slot_import_cb_t cb);

// Update the small notice line below the import button. Pass NULL or ""
// to clear it. Used by main.cpp to surface the import result —
// "Imported N fields" or an error string.
void ui_slot_detail_set_import_notice(const char* text);

// ── Scale settings + calibration wizard ────────────────────────
// Reachable by tapping the Scale card on the home screen. Hosts a
// "Tare", "Add point" (opens the wizard), "Clear" and "Close" set of
// buttons; live weight readout up top; a status line that reflects the
// scale's current calibration state ("Calibration: N points" or
// "Uncalibrated"). The screen + the wizard share the same tap callback
// for buttons to keep main.cpp's wiring trivial.

// Tap target on the home-screen Scale card. Fires when the user taps
// the scale card; main.cpp typically responds with `ui_show_scale_settings()`.
typedef void (*ui_scale_tap_cb_t)(void);
void ui_set_scale_settings_tap_callback(ui_scale_tap_cb_t cb);

// Open the scale-settings screen. Idempotent.
void ui_show_scale_settings();
// True while the scale-settings screen OR its calibration wizard is
// the currently-loaded LVGL screen — main.cpp uses this to gate the
// live-weight forwarding so we don't churn the readout when the user
// isn't looking at it.
bool ui_scale_settings_visible();

// Push a fresh weight reading into the screen + wizard. `state` is
// "new" / "stable" / "unstable" / "removed" / "uncalibrated" — same
// vocabulary main.cpp already gets from g_scale.onWeight. precision
// is 0..4 decimals.
void ui_set_scale_settings_live_weight(float grams, const char* state, int precision);
// Push a CalibrationStatus snapshot. `tared` is true when the scale
// has been zeroed at least once (CalibrationStatus.tare_raw != 0).
void ui_set_scale_settings_status(int num_points, bool tared);

// Buttons on the scale-settings screen. CLOSE returns to home;
// the rest forward the action to main.cpp which talks to ScaleLink.
typedef enum {
    SCALE_BTN_TARE      = 1,
    SCALE_BTN_ADD_POINT = 2,
    SCALE_BTN_CLEAR     = 3,
    SCALE_BTN_CLOSE     = 4,
} scale_btn_t;
typedef void (*scale_settings_cb_t)(scale_btn_t action);
void ui_set_scale_settings_callback(scale_settings_cb_t cb);

// Calibration wizard — a 3-step flow opened by SCALE_BTN_ADD_POINT.
// `presets` is the list of weight chips the wizard renders on step 1.
// Wizard owns its own state until the user finishes / cancels, at
// which point it returns to the scale-settings screen.
void ui_start_calibration_wizard(const int* presets, size_t n);
// True while the wizard is the currently-loaded screen. main.cpp uses
// this to forward weight events into the wizard's stable-detect logic.
bool ui_calibration_wizard_visible();
// Push a weight reading into the wizard's stable-detect ladder.
// Capture-button enable state is recomputed inside.
void ui_calibration_wizard_on_weight(float grams, const char* state, int precision);
// Fired when the user taps Capture on step 2; `weight` is the preset
// they picked on step 1. main.cpp issues g_scale.addCalPoint(weight).
typedef void (*ui_calibration_capture_cb_t)(int weight);
void ui_set_calibration_capture_callback(ui_calibration_capture_cb_t cb);

// Fired when the wizard advances from step 1 (pick) to step 2
// (capture). main.cpp typically responds by calling
// `g_scale.requestCurrentWeight()` so the wizard's gating logic
// re-runs on a fresh weight event. Necessary because the scale
// doesn't periodically re-emit "Uncalibrated" — without a fresh
// reply the Capture button can never enable on a freshly-cleared
// scale unless the original Add-point request's reply happened to
// arrive while step 2 was already visible.
typedef void (*ui_calibration_step_enter_cb_t)(void);
void ui_set_calibration_capture_enter_callback(ui_calibration_step_enter_cb_t cb);

// ── Device config screen (Settings / Info tabs) ────────────────
// Reached by tapping the home footer strip. Two tabs toggled by header
// buttons: Settings (sleep-timeout presets + update/restart actions) and
// Info (read-only device status). Tap-only — no text entry.

// Settings-tab action buttons. Sleep presets carry their value via the
// dedicated sleep callback, so they're not in this enum.
typedef enum {
    DEVCFG_BTN_CHECK_UPDATES = 1,   // check BOTH console + paired scale
    DEVCFG_BTN_APPLY_CONSOLE = 2,   // apply console update (→ confirm → OTA)
    DEVCFG_BTN_APPLY_SCALE   = 3,   // apply scale update   (→ confirm → OTA)
    DEVCFG_BTN_RESTART       = 4,   // restart console      (→ confirm)
} devcfg_btn_t;
typedef void (*devcfg_action_cb_t)(devcfg_btn_t action);
void ui_set_devcfg_action_callback(devcfg_action_cb_t cb);

// Fired when a sleep-timeout preset chip is tapped. `seconds` is the
// preset (0 = never). main.cpp calls
// ConsoleDisplay::setAndPersistSleepTimeout(seconds) + pushDisplayConfig().
typedef void (*devcfg_sleep_cb_t)(uint32_t seconds);
void ui_set_devcfg_sleep_callback(devcfg_sleep_cb_t cb);

// Fired when the user taps the home footer strip. main.cpp responds by
// gathering the current identity and calling ui_show_device_config(...).
typedef void (*ui_devcfg_open_cb_t)(void);
void ui_set_devcfg_open_callback(ui_devcfg_open_cb_t cb);

// Open the screen. Static identity fields (stable while open) are passed
// once here; live fields arrive via ui_set_devcfg_live(). `startOnInfo`
// opens directly on the Info tab (used when routing back after a scale
// update completes so the user sees the new scale version); false opens
// on Settings.
void ui_show_device_config(const char* hostname, const char* ip,
                           const char* fw_version, const char* fe_version,
                           bool startOnInfo);
// True while the device-config screen is the active LVGL screen — main.cpp
// gates the ~1 Hz live push behind it.
bool ui_devcfg_visible();

// Live state pushed ~1 Hz from main.cpp's loop while ui_devcfg_visible().
// Drives the sleep-preset highlight, heap/scale Info rows, the OTA status
// line, and the conditional Apply buttons.
struct UiDevcfgLive {
    uint32_t    free_heap_b;        // ESP.getFreeHeap()
    bool        scale_linked;
    const char* scale_version;      // "" when unknown / not linked
    uint32_t    sleep_timeout_s;    // current applied value (chip highlight)
    bool        ota_checking;       // checkInFlight on console or scale
    bool        console_update;     // console firmware/frontend pending
    const char* console_latest;     // version string for the Apply button
    bool        scale_update;       // scale pending AND scale linked
    const char* scale_latest;
    const char* check_status;       // "" | "ok" | "network" | "http_error" | "parse_error"
    int         check_age_s;        // seconds since last check, -1 = never
};
void ui_set_devcfg_live(const UiDevcfgLive& live);
