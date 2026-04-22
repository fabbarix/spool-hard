#include <Arduino.h>
#include <LittleFS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <Preferences.h>

#include "config.h"
#include "display.h"
#include "wifi_provisioning.h"
#include "web_server.h"
#include "scale_link.h"
#include "nfc_reader.h"
#include "store.h"
#include "ui/ui.h"
#include "ui/ui_wizard.h"
#include "protocol.h"
#include "spoolhard/ota.h"
#include "sdcard.h"
#include "bambu_manager.h"
#include "bambu_cloud.h"
#include "bambu_discovery.h"
#include "core_weights.h"
#include "pending_ams.h"
#include <lvgl.h>   // LV_SYMBOL_* FontAwesome glyphs baked into Montserrat
#include "scale_discovery.h"
#include "ssdp_hub.h"

// ── Globals ──────────────────────────────────────────────────
static ConsoleDisplay    g_display;
static WifiProvisioning  g_wifi;
ConsoleWebServer         g_web;   // extern-visible so bambu_printer.cpp can broadcast debug frames
ScaleLink                g_scale;   // extern-visible for bambu_printer.cpp (pushGcodeAnalysis)
static NfcReader         g_nfc;
SpoolStore               g_store;   // extern-visible for bambu_printer.cpp (AMS → spool lookup)

static bool    g_pendingOta = false;
static bool    g_showedHome = false;

// Spool-detail screen state. Set when a known tag is scanned on either the
// on-device PN532 or via the scale. Used by:
//   • scale-button handler — to know which spool to apply "capture current
//     weight" to when the user presses the scale button with a stable load.
//   • AMS-assigned callback — to auto-dismiss the LCD spool-detail screen
//     a couple seconds after the scanned spool is confirmed in a tray.
// Cleared when the user leaves the spool screen (any button) or when the
// active window expires (SPOOL_CTX_TIMEOUT_MS). Tracked as a plain String
// because SpoolRecord ids are short.
static String   g_activeSpoolId;
static uint32_t g_activeSpoolOpenedMs  = 0;
static uint32_t g_activeSpoolExpiresAt = 0;  // millis: AMS-load timeout deadline
static uint32_t g_spoolAutoCloseAt     = 0;  // millis: loop() should ui_show_home() when crossed
static constexpr uint32_t SPOOL_AUTOCLOSE_MS = 2500;  // confirmation dwell after AMS assign

// Render the spool-detail LCD screen from a SpoolRecord. Pure UI — doesn't
// touch PendingAms or the active-spool context trackers, so callers can use
// this both to open the screen after a tag scan AND to refresh it after a
// weight capture without reshuffling state.
static void showSpoolDetail(const SpoolRecord& rec) {
    String title = rec.brand.length() ? rec.brand : String("Unknown");
    if (rec.material_type.length()) title += " " + rec.material_type;
    String subtitle = rec.color_name.length() ? rec.color_name : String("");
    if (subtitle.length()) subtitle += "  " LV_SYMBOL_BULLET "  ";
    subtitle += "tag " + rec.tag_id;
    ui_show_spool(rec.id.c_str(),
                  title.c_str(),
                  subtitle.c_str(),
                  rec.color_code.c_str(),
                  rec.weight_current,
                  rec.weight_advertised,
                  rec.weight_core,
                  rec.weight_new,
                  (int)rec.consumed_since_weight);
    // Seed the live-weight strip from the cached scale reading so the user
    // doesn't see a stale "--" until the next scale state change.
    if (g_scale.lastWeightMs() > 0) {
        ui_set_spool_live_weight(g_scale.lastWeightG(),
                                 g_scale.lastWeightState().c_str());
    } else {
        ui_set_spool_live_weight(0.f, nullptr);
    }
}

// Apply "capture current weight" to a known spool, mirroring the LCD button
// path: subtract the known/learned core weight, reset consumed_since_weight,
// persist. Returns false when we can't determine a valid net weight (no
// stable reading / unknown spool). No-op when the state isn't right.
static bool captureCurrentWeightForSpool(const String& spool_id) {
    if (spool_id.isEmpty()) return false;
    SpoolRecord rec;
    if (!g_store.findById(spool_id, rec)) {
        Serial.printf("[Spool] capture: unknown id %s\n", spool_id.c_str());
        return false;
    }
    if (!g_scale.hasStableWeight()) {
        Serial.println("[Spool] capture: no stable scale reading");
        return false;
    }
    int g = (int)g_scale.lastWeightG();
    if (g <= 0) {
        Serial.printf("[Spool] capture: invalid weight %dg\n", g);
        return false;
    }
    int core = rec.weight_core;
    if (core <= 0) {
        int learned = CoreWeights::get(rec.brand, rec.material_type,
                                       rec.weight_advertised);
        if (learned > 0) core = learned;
    }
    int net = g;
    if (core > 0) {
        net = g - core;
        if (net < 0) net = 0;
        Serial.printf("[Spool] capture: scale=%dg - core=%dg → %dg\n",
                      g, core, net);
    } else {
        Serial.printf("[Spool] capture: no core weight known, "
                      "storing raw scale=%dg (capture empty first!)\n", g);
    }
    rec.weight_current        = net;
    rec.consumed_since_weight = 0;
    g_store.upsert(rec);
    Serial.printf("[Spool] captured weight_current=%dg for %s\n",
                  net, rec.id.c_str());
    return true;
}

// Forward-declared — setup() registers the banner tap callback before
// the helper is defined further down (next to _refreshOtaBanner).
static void _onOtaBannerTap();

// ── Setup ────────────────────────────────────────────────────
void setup() {
    Serial.begin(DEBUG_BAUD);
    delay(200);
    Serial.printf("\n[Main] SpoolHardConsole fw %s starting\n", FW_VERSION);

    // Filesystems
    if (!SPIFFS.begin(true)) {
        Serial.println("[Main] SPIFFS mount failed");
    }
    if (!LittleFS.begin(true, USERFS_MOUNT, 10, USERFS_LABEL)) {
        Serial.println("[Main] LittleFS (userfs) mount failed");
    }
    g_sd.begin();
    g_bambu.begin();
    g_bambu_cloud.begin();
    // SNTP — gives the OTA checker a real wall-clock for "last
    // checked at …" timestamps. Falls back to 0 if WiFi isn't up
    // yet; the checker handles that.
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    g_ota_checker.begin();
    // g_bambu_discovery starts after WiFi is initialised (see below).

    // Display + LVGL (task is pinned to Core 1 inside begin()).
    g_display.begin();
    ui_init();
    ui_show_splash();

    // Apply the persisted screen-sleep timeout (NVS, seconds). 0 = off.
    {
        Preferences p;
        p.begin(NVS_NS_DISPLAY, true);
        uint32_t s = p.getUInt(NVS_KEY_DISP_SLEEP_S, DEFAULT_DISP_SLEEP_S);
        p.end();
        ConsoleDisplay::setSleepTimeout(s);
    }

    // Load spool DB.
    g_store.begin();

    // Bring up HTTP server + WiFi provisioning.
    g_web.setStore(&g_store);
    g_web.setScaleLink(&g_scale);
    // Map the upload-handler's lower-case `type` ("firmware" / "spiffs") to
    // the title shown on the LCD. Kept inline here rather than in the web
    // handler so the device-vs-network vocabulary stays separate.
    auto _otaTitleFor = [](const char* type) -> const char* {
        if (type && !strcmp(type, "spiffs")) return "Updating Frontend";
        return "Updating Firmware";
    };
    g_web.onUploadStarted([_otaTitleFor](const char* type) {
        ConsoleDisplay::wake();     // make sure the user sees the progress
        ui_show_ota_progress(0, _otaTitleFor(type), type);
    });
    g_web.onUploadProgress([_otaTitleFor](int percent, const char* type, const char* label) {
        // `label` starts as the filename and switches to "v<version>" once
        // the version marker is parsed; passing it each tick means the
        // screen text updates from "firmware.bin" to "v0.1.0.alpha-N"
        // mid-upload without any extra plumbing.
        ConsoleDisplay::wake();
        ui_show_ota_progress(percent, _otaTitleFor(type), label);
    });
    g_web.onOtaRequested([]() { g_pendingOta = true; });
    ui_set_ota_banner_callback(_onOtaBannerTap);
    g_web.begin();
    g_wifi.begin(g_web.server());
    g_web.start();

    // Populate onboarding screen with the values we know at boot.
    ui_set_onboarding(g_wifi.getApSsid().c_str(),
                      g_wifi.getSecurityKey().c_str(),
                      "192.168.4.1");

    // Shared SSDP hub: one socket per port, many subscribers. Has to happen
    // after WiFi.begin() — AsyncUDP.listenMulticast silently fails without
    // an interface.
    ssdp_hub_begin();
    g_bambu_discovery.begin();
    g_scale_discovery.begin();

    // Scale link: SSDP-discovered, WebSocket client.
    g_scale.begin();
    g_scale.onConnect([]() {
        JsonDocument d; d["connected"] = true;
        g_web.broadcastDebug("scale_link", d);
        // Pull the scale's current weight immediately. The scale only
        // emits LoadChanged* frames on state transitions, so if the scale
        // has been sitting stable with a load on it since before we
        // connected, we'd never hear about that load otherwise — both the
        // home-screen weight card and the spool-detail live readout would
        // be frozen at "0.0 g" until the user disturbed the scale.
        g_scale.requestCurrentWeight();
    });
    g_scale.onDisconnect([]() {
        JsonDocument d; d["connected"] = false;
        g_web.broadcastDebug("scale_link", d);
    });
    // Drive the LCD scale-status widget from the richer handshake state so
    // "offline", "no key", and "online" all render distinctly — the old
    // binary connected/disconnected was silently showing "Scale online"
    // whenever a WS had ever connected, even after the link dropped.
    // Map the link-layer handshake into the UI's four-colour LED. The
    // "disconnected / failed" cases split further by whether we have a
    // paired scale name: with a name it's "discovering" (orange — we know
    // which scale to reach, just haven't right now), without a name it's
    // "missing" (red — nothing paired at all).
    auto mapScaleState = [](ScaleLink::Handshake h, const String& name) {
        switch (h) {
            case ScaleLink::Handshake::Encrypted:   return SCALE_LCD_ENCRYPTED;
            case ScaleLink::Handshake::Unencrypted: return SCALE_LCD_UNENCRYPTED;
            case ScaleLink::Handshake::Failed:
            default:
                return name.length() ? SCALE_LCD_DISCOVERING : SCALE_LCD_MISSING;
        }
    };
    g_scale.onHandshakeChanged([=](ScaleLink::Handshake h, const String& name) {
        ui_set_scale_state(mapScaleState(h, name), name.c_str());
    });
    // Seed the LCD so the widget isn't stuck on its placeholder between
    // boot and the first WS event. "Missing" (red) if nothing paired yet,
    // "Discovering" (orange) if a pairing exists but we haven't reached
    // the scale yet.
    ui_set_scale_state(mapScaleState(g_scale.handshake(), g_scale.scaleName()),
                       g_scale.scaleName().c_str());
    g_scale.onWeight([](float grams, const char* state) {
        // If the display is asleep, wake it so the user sees the reading.
        // DON'T reset the idle timer if it's already on — the scale pushes
        // unstable/stable transitions on its own cadence and was pinning
        // the display on regardless of sleep_timeout_s. Touch still resets
        // the timer the normal way for any real user interaction.
        ConsoleDisplay::wakeIfAsleep();
        int precision = g_scale.scalePrecision();
        ui_set_weight(grams, state, precision);
        // Also drive the spool screen's live readout when it's the active
        // screen — user can watch the reading settle before tapping capture.
        ui_set_spool_live_weight(grams, state);
        // Same goes for the registration wizard's Full/Used/Empty screens.
        ui_wizard_on_weight(grams, state);
        JsonDocument d;
        d["weight_g"]  = grams;
        d["state"]     = state;
        d["precision"] = precision;
        g_web.broadcastDebug("weight", d);
    });
    // Shared entry point for tags read either from the console's own PN532
    // or forwarded from the scale's NFC reader over the WebSocket. Behaviour
    // is identical from the user's perspective: known tag → spool-detail
    // screen, unknown tag → (today) minimal stub + detail screen; (future,
    // task 37) → registration wizard.
    auto handleTagRead = [](const SpoolTag& tag, const char* source) {
        ConsoleDisplay::wake();
        Serial.printf("[%s] tag %s format=%s url=%s\n",
                      source, tag.uid_hex.c_str(), tag.format.c_str(), tag.ndef_url.c_str());
        ui_set_last_tag(tag.uid_hex.c_str(), tag.ndef_url.c_str());

        // Known tag → straight to the detail screen (no regression).
        // Unknown tag → registration wizard (new). The wizard itself handles
        // the "drop second scan while active" case.
        SpoolRecord rec;
        bool is_new = !g_store.findByTagId(tag.uid_hex, rec);

        if (is_new) {
            ui_wizard_start(tag);
        } else {
            showSpoolDetail(rec);
            g_scale.requestCurrentWeight();

            // Arm the AMS auto-assignment latch. BambuPrinter::_parseAms will
            // consume it the next time it sees any connected printer's tray
            // transition from empty → populated within the 2-minute window,
            // push this spool's tray metadata there, and persist the slot
            // mapping in the per-printer override table.
            PendingAms::arm(rec.id);
            ui_set_spool_ams_status(LV_SYMBOL_REFRESH "  AMS: waiting for load…");

            // Active-spool context: the scale button can now "capture current
            // weight" on this spool, and the screen stays open until EITHER
            // a Bambu AMS tray load triggers the assigned callback OR the
            // PendingAms expiry window elapses with no load. Mirror the
            // PendingAms timeout locally so we close the screen on timeout
            // without racing the PendingAms claim path.
            g_activeSpoolId        = rec.id;
            g_activeSpoolOpenedMs  = millis();
            g_activeSpoolExpiresAt = millis() + PendingAms::kDefaultExpiryMs;
            g_spoolAutoCloseAt     = 0;
        }

        JsonDocument d;
        d["uid"]    = tag.uid_hex;
        d["url"]    = tag.ndef_url;
        d["format"] = tag.format;
        d["source"] = source;
        d["new"]    = is_new;
        g_web.broadcastDebug("nfc", d);
    };
    // Capture the helper into both callback lambdas via [=] — it holds no
    // non-capturable state and is cheap to copy.
    g_scale.onTag([handleTagRead](const char* uid, const char* url, bool bambu) {
        // Scale forwards raw UID + URL + a "looks-like-Bambu" flag. We
        // manufacture a SpoolTag locally so the rest of the flow doesn't
        // care which reader produced it.
        SpoolTag tag;
        tag.uid_hex  = uid ? uid : "";
        tag.ndef_url = url ? url : "";
        tag.format   = bambu ? "BambuLab" : "Unknown";
        SpoolTag::parseUrl(tag.ndef_url, tag);   // refines format + parsed_*
        handleTagRead(tag, "Scale");
    });

    // Scale physical button → "capture current weight" for the spool the
    // user just scanned on the scale. Requires an active spool context and
    // a stable scale reading; mirrors the LCD's CAPTURE_CURRENT button
    // path so there's one source of truth (captureCurrentWeightForSpool).
    g_scale.onButton([]() {
        if (g_activeSpoolId.isEmpty()) {
            Serial.println("[ScaleBtn] pressed — no active spool, ignored");
            g_scale.sendButtonResponse(false);
            return;
        }
        // If the AMS-load context has already expired, the spool-detail
        // screen is about to be dismissed by the loop() timeout check;
        // don't start a fresh capture right at the deadline.
        if ((int32_t)(millis() - g_activeSpoolExpiresAt) >= 0) {
            Serial.println("[ScaleBtn] pressed — spool context expired, ignored");
            g_scale.sendButtonResponse(false);
            return;
        }
        Serial.printf("[ScaleBtn] pressed — capture on %s\n",
                      g_activeSpoolId.c_str());
        if (captureCurrentWeightForSpool(g_activeSpoolId)) {
            // Refresh the on-screen values from the updated record — the
            // user sees the new weight_current immediately — and leave the
            // screen open so they can keep watching for the AMS load.
            // The screen will close on either the AMS-assigned callback
            // (2.5 s after claim) or the PendingAms expiry (handled in
            // loop()), whichever comes first.
            SpoolRecord rec;
            if (g_store.findById(g_activeSpoolId, rec)) {
                showSpoolDetail(rec);
                // showSpoolDetail calls ui_show_spool which resets the
                // AMS status line; restore the "waiting for load…" hint
                // so the user doesn't think the latch got cleared.
                ui_set_spool_ams_status(LV_SYMBOL_REFRESH "  AMS: waiting for load…");
                // Briefly flash the freshly-captured weight so the user's
                // eye is drawn to the number that just changed.
                ui_flash_spool_current();
            }
            // ACK the scale so its RGB LED plays the "capture ok" pattern.
            g_scale.sendButtonResponse(true);
        } else {
            // No stable reading / unknown record / zero weight — tell the
            // scale so it plays the "capture fail" pattern instead.
            g_scale.sendButtonResponse(false);
        }
    });

    // BambuPrinter notifies us when a scanned spool has just been auto-
    // assigned to an AMS tray. Schedule an auto-close of the spool-detail
    // screen so the user sees the confirmation banner briefly (already
    // applied in ui_set_spool_ams_status) before returning home.
    BambuPrinter::setOnSpoolAssigned([](const String& spool_id,
                                        const String& printer,
                                        int ams_unit, int slot_id) {
        Serial.printf("[Spool] %s assigned to %s AMS %d slot %d\n",
                      spool_id.c_str(), printer.c_str(), ams_unit, slot_id);
        if (g_activeSpoolId.isEmpty() || g_activeSpoolId != spool_id) return;
        g_spoolAutoCloseAt = millis() + SPOOL_AUTOCLOSE_MS;
    });

    // On-device PN532.
    g_nfc.begin();
    g_nfc.onTag([handleTagRead](const SpoolTag& tag) {
        handleTagRead(tag, "NFC");
    });

    // Button callback from the spool screen. CAPTURE_CURRENT stores the
    // filament-only weight (scale reading minus the empty-core weight) into
    // weight_current and resets consumed_since_weight; CAPTURE_EMPTY stamps
    // weight_core; CLOSE just returns to home. All three leave the spool
    // record id intact.
    ui_set_spool_callback([](spool_btn_t action, const char* spool_id) {
        if (action == SPOOL_BTN_CLOSE) {
            g_activeSpoolId = "";
            g_spoolAutoCloseAt = 0;
            ui_show_home();
            return;
        }
        if (action == SPOOL_BTN_CAPTURE_CURRENT) {
            if (!captureCurrentWeightForSpool(String(spool_id))) {
                // Helper already logged the reason; if it was
                // "no stable reading" stay on screen so the live
                // readout keeps updating. For missing-record go home.
                SpoolRecord rec;
                if (!g_store.findById(String(spool_id), rec)) {
                    g_activeSpoolId = "";
                    g_spoolAutoCloseAt = 0;
                    ui_show_home();
                }
                return;
            }
        } else if (action == SPOOL_BTN_CAPTURE_EMPTY) {
            SpoolRecord rec;
            if (!g_store.findById(String(spool_id), rec)) {
                g_activeSpoolId = "";
                g_spoolAutoCloseAt = 0;
                ui_show_home();
                return;
            }
            if (!g_scale.hasStableWeight()) {
                Serial.println("[Spool] capture-empty aborted: no stable reading");
                return;
            }
            int g = (int)g_scale.lastWeightG();
            rec.weight_core = g;
            if (!rec.brand.isEmpty() && !rec.material_type.isEmpty() &&
                rec.weight_advertised > 0) {
                CoreWeights::set(rec.brand, rec.material_type, rec.weight_advertised, g);
            }
            g_store.upsert(rec);
            Serial.printf("[Spool] captured weight_core=%dg for %s\n", g, rec.id.c_str());
        }
        g_activeSpoolId = "";
        g_spoolAutoCloseAt = 0;
        ui_show_home();
    });

    // Initial screen
    if (g_wifi.getState() == WifiState::Unconfigured ||
        g_wifi.getState() == WifiState::Failed) {
        ui_show_onboarding();
    } else {
        ui_show_home();
        g_showedHome = true;
    }

    // Tap-on-tile → slot detail screen. We build the UiSlotDetail from
    // live state here (rather than caching it in the UI layer) so the
    // user always sees fresh data when opening the screen. Looks at AMS
    // unit 0's trays for slots 0..3 and the external vt_tray for slot 4.
    ui_set_slot_tap_callback([](int slot_idx) {
        UiSlotDetail d = {};
        const BambuPrinter* bp = nullptr;
        for (const auto& pp : g_bambu.printers()) {
            if (pp && pp->state().link == BambuLinkState::Connected) { bp = pp.get(); break; }
        }
        if (!bp) {
            // Nothing to show — stay on home.
            return;
        }
        const PrinterState& s = bp->state();
        const char* name = bp->config().name.length() ? bp->config().name.c_str()
                                                      : bp->config().serial.c_str();
        snprintf(d.printer_name, sizeof(d.printer_name), "%s", name);

        // Resolve the AmsTray this slot refers to. Slots 0..3 map to AMS
        // unit 0's trays; slot 4 is the external spool holder (vt_tray).
        const AmsTray* tr = nullptr;
        if (slot_idx >= 0 && slot_idx < 4) {
            if (s.ams_count > 0) tr = &s.ams[0].trays[slot_idx];
            snprintf(d.slot_label, sizeof(d.slot_label), "AMS 1.%d", slot_idx + 1);
        } else if (slot_idx == 4) {
            if (s.has_vt_tray) tr = &s.vt_tray;
            snprintf(d.slot_label, sizeof(d.slot_label), "Ext");
        } else {
            return;
        }

        if (tr) {
            d.active = (s.active_tray == tr->id);
            snprintf(d.material, sizeof(d.material), "%s", tr->tray_type.c_str());
            if (tr->tray_color.length() >= 6) {
                d.color_rgb = (uint32_t)strtoul(
                    tr->tray_color.substring(0, 6).c_str(), nullptr, 16);
            }
            d.remain_pct          = tr->remain_pct;
            d.k                   = tr->k;
            d.cali_idx            = tr->cali_idx;
            d.mapped_via_override = tr->mapped_via_override;
            d.ams_nozzle_min_c    = tr->nozzle_min_c;
            d.ams_nozzle_max_c    = tr->nozzle_max_c;
            snprintf(d.ams_tray_info_idx, sizeof(d.ams_tray_info_idx), "%s",
                     tr->tray_info_idx.c_str());
            snprintf(d.tag_uid, sizeof(d.tag_uid), "%s", tr->tag_uid.c_str());

            if (tr->mapped_spool_id.length()) {
                SpoolRecord rec;
                if (g_store.findById(tr->mapped_spool_id, rec)) {
                    d.has_spool = true;
                    snprintf(d.spool_id,         sizeof(d.spool_id),         "%s", rec.id.c_str());
                    snprintf(d.spool_tag_id,     sizeof(d.spool_tag_id),     "%s", rec.tag_id.c_str());
                    snprintf(d.brand,            sizeof(d.brand),            "%s", rec.brand.c_str());
                    snprintf(d.material_subtype, sizeof(d.material_subtype), "%s", rec.material_subtype.c_str());
                    snprintf(d.color_name,       sizeof(d.color_name),       "%s", rec.color_name.c_str());
                    // Mirror the material onto the record's type if the AMS
                    // didn't report one (SpoolHard-tagged spools often don't).
                    if (!d.material[0] && rec.material_type.length()) {
                        snprintf(d.material, sizeof(d.material), "%s", rec.material_type.c_str());
                    }
                    // color_code: store just the 6-hex RRGGBB — strtoul handles
                    // that in the UI layer.
                    snprintf(d.color_code, sizeof(d.color_code), "%s",
                             rec.color_code.length() >= 6
                                 ? rec.color_code.substring(0, 6).c_str()
                                 : "");
                    d.weight_current_g        = rec.weight_current;
                    d.weight_advertised_g     = rec.weight_advertised;
                    d.weight_core_g           = rec.weight_core;
                    d.weight_new_g            = rec.weight_new;
                    d.consumed_since_weight_g = rec.consumed_since_weight;
                    d.nozzle_temp_min         = rec.nozzle_temp_min;
                    d.nozzle_temp_max         = rec.nozzle_temp_max;
                    d.density                 = rec.density;
                    snprintf(d.slicer_filament, sizeof(d.slicer_filament), "%s", rec.slicer_filament.c_str());
                    snprintf(d.note,            sizeof(d.note),            "%s", rec.note.c_str());
                }
            }
        } else {
            // Slot not reported (e.g. AMS 0 missing, or external tray not
            // present on this printer). Keep label + printer_name; the UI
            // will render "Empty slot".
        }

        ui_show_slot_detail(&d);
    });

    Serial.println("[Main] Init complete");
}

// Refresh the home-screen footer strip (hostname + IP). Needed as a
// periodic tick rather than a one-shot setup() call because the device
// boots into `ui_show_home()` directly when WiFi credentials are already
// stored — the onboarding → home transition block in loop() below never
// fires, so the footer labels would otherwise stay on their "—"
// placeholders forever.
static void _refreshHomeFooter() {
    // Map the firmware-facing WifiState into the UI's four-state enum.
    // Unconfigured/Failed both drive the provisioning SoftAP (orange);
    // Connecting is yellow; Connected is green; anything else renders
    // muted grey.
    wifi_lcd_state_t wifi_state = WIFI_LCD_DISCONNECTED;
    switch (g_wifi.getState()) {
        case WifiState::Connected:    wifi_state = WIFI_LCD_CONNECTED;   break;
        case WifiState::Connecting:   wifi_state = WIFI_LCD_CONNECTING;  break;
        case WifiState::Unconfigured:
        case WifiState::Failed:       wifi_state = WIFI_LCD_AP;          break;
    }

    // Hostname tracks the device name regardless of link state — it's
    // still useful info (next boot it'll be reachable at that name).
    // mDNS normalisation: lowercase, spaces→dashes, + ".local".
    String host = g_wifi.getDeviceName();
    host.toLowerCase();
    host.replace(' ', '-');
    if (host.length()) host += ".local";
    ui_set_hostname(host.c_str(), wifi_state);

    // IP only makes sense when the STA interface is up.
    if (WiFi.status() == WL_CONNECTED) {
        static String s_last_ip;
        String ip = WiFi.localIP().toString();
        if (ip != s_last_ip) {
            ui_set_ip(ip.c_str());
            s_last_ip = ip;
        }
    }
}

// Refresh the home-screen AMS panel from the first connected Bambu printer.
// Called once per second from loop(). Kept deliberately simple: walk the
// printer list, pick the first one whose link is Connected, snapshot slots
// 0..3 of AMS unit 0 + the external tray, and hand the array to the UI.
//
// Multi-printer setups see only the first connected printer here — the web
// dashboard shows all of them. The LCD is a glance-value aimed at "what's in
// front of me right now"; cycling between printers on the LCD is a future
// nicety.
static void _refreshHomeAmsPanel() {
    for (const auto& pp : g_bambu.printers()) {
        if (!pp) continue;
        const PrinterState& s = pp->state();
        if (s.link != BambuLinkState::Connected) continue;

        // The printer card above the AMS panel now carries the name/state
        // line. We still pass a non-empty `status` into ui_set_ams_panel so
        // it un-hides the AMS card — the label itself is hidden internally.
        const char* name = pp->config().name.length() ? pp->config().name.c_str()
                                                       : pp->config().serial.c_str();
        const char* status = name;

        auto fillSlot = [&](UiAmsSlot& dst, const AmsTray& tr, const char* label) {
            snprintf(dst.label,    sizeof(dst.label),    "%s", label);
            snprintf(dst.material, sizeof(dst.material), "%s", tr.tray_type.c_str());
            dst.remain_pct = tr.remain_pct;
            dst.k          = tr.k;
            dst.active     = (s.active_tray == tr.id);

            // Colour source priority: AMS-reported tray_color wins when it's
            // known and non-zero — it's the ground truth of what's
            // physically loaded RIGHT NOW (the printer's RFID read, or what
            // the user set on its panel). The mapped SpoolRecord is only
            // the fallback, for the SpoolHard-tagged case before our
            // auto-push lands and for slots the printer hasn't identified.
            // Treat "000000..." as "unknown" rather than real black —
            // Bambu reports all zeros for empty/unset trays.
            SpoolRecord rec;
            bool have_rec = tr.mapped_spool_id.length() &&
                            g_store.findById(tr.mapped_spool_id, rec);
            uint32_t ams_rgb = 0;
            if (tr.tray_color.length() >= 6) {
                ams_rgb = (uint32_t)strtoul(
                    tr.tray_color.substring(0, 6).c_str(), nullptr, 16);
            }
            if (ams_rgb != 0) {
                dst.color_rgb = ams_rgb;
            } else if (have_rec && rec.color_code.length() >= 6) {
                dst.color_rgb = (uint32_t)strtoul(
                    rec.color_code.substring(0, 6).c_str(), nullptr, 16);
            } else {
                dst.color_rgb = 0;
            }

            // "Occupied" = printer has *some* metadata, OR we have a mapped
            // spool record (covers SpoolHard-tagged spools that report no
            // tag_uid/tray_type to the printer yet).
            dst.occupied = tr.tray_type.length() || tr.tray_color.length() ||
                           tr.tag_uid.length()   || have_rec;
            dst.weight_g = (have_rec && rec.weight_current >= 0) ? rec.weight_current : -1;
            // When we're sourcing from the spool record, backfill the
            // material name so the tile labels "PLA" rather than "" for a
            // SpoolHard spool the printer doesn't have tray_type for yet.
            if (have_rec && dst.material[0] == '\0' && rec.material_type.length()) {
                snprintf(dst.material, sizeof(dst.material), "%s", rec.material_type.c_str());
            }
        };

        UiAmsSlot slots[5] = {};
        // AMS unit 0 first four trays (covers X1/P1/A1/P1S — H2D owners see
        // only their first AMS unit here, by design). Labels are rendered
        // 1-indexed for humans ("1.1".."1.4"); internal array indexing
        // stays 0-based.
        char lbl[8];
        if (s.ams_count > 0) {
            for (int t = 0; t < 4; ++t) {
                const AmsTray& tr = s.ams[0].trays[t];
                snprintf(lbl, sizeof(lbl), "1.%d", t + 1);
                fillSlot(slots[t], tr, lbl);
            }
        } else {
            for (int t = 0; t < 4; ++t) {
                snprintf(lbl, sizeof(lbl), "1.%d", t + 1);
                snprintf(slots[t].label, sizeof(slots[t].label), "%s", lbl);
            }
        }
        // External (vt_tray).
        if (s.has_vt_tray) {
            fillSlot(slots[4], s.vt_tray, "Ext");
        } else {
            snprintf(slots[4].label, sizeof(slots[4].label), "Ext");
        }

        ui_set_ams_panel(status, slots);

        // Build + push the printer-card snapshot so the LCD's new strip
        // above the AMS tiles shows link state, gcode state, progress bar,
        // layer count and live temps for the same printer. We show only
        // one printer at a time on the LCD by design.
        UiPrinterSnapshot snap = {};
        snap.connected = true;
        snprintf(snap.name, sizeof(snap.name), "%s", name);
        snprintf(snap.gcode_state, sizeof(snap.gcode_state), "%s", s.gcode_state.c_str());
        snap.progress_pct = s.progress_pct;
        snap.layer_num    = s.layer_num;
        snap.total_layers = s.total_layers;
        snap.nozzle_temp  = s.nozzle_temp;
        snap.bed_temp     = s.bed_temp;
        // has_temps = true once we've seen any reading. The first MQTT
        // report populates both, so "either is positive" is a reliable
        // "we have real data" proxy.
        snap.has_temps    = (s.nozzle_temp > 0.f) || (s.bed_temp > 0.f);
        ui_set_printer_panel(&snap);
        return;
    }
    // No connected printer — hide both panels.
    ui_set_ams_panel(nullptr, nullptr);
    ui_set_printer_panel(nullptr);
}

// Combined OTA-pending banner refresh. Reads the console's own pending
// state from g_ota_checker plus the cached scale push (g_scale.scaleOtaPending)
// and reflects either / both on the LCD home screen. Hides the banner when
// nothing is pending. Cheap — one LVGL call only when the message text
// actually changed.
static void _refreshOtaBanner() {
    auto cp = g_ota_checker.pending();
    const auto& sp = g_scale.scaleOtaPending();
    bool consolePending = cp.firmware || cp.frontend;
    bool scalePending   = sp.valid && (sp.firmware_update || sp.frontend_update);

    static String _lastText;
    if (!consolePending && !scalePending) {
        if (!_lastText.isEmpty()) {
            ui_set_ota_banner(false, nullptr);
            _lastText = "";
        }
        return;
    }

    String text;
    if (consolePending && scalePending) {
        text = "Console " + cp.firmware_latest +
               " + Scale "  + sp.firmware_latest +
               " — tap to install";
    } else if (consolePending) {
        text = "Console " + cp.firmware_latest + " available — tap to install";
    } else {
        text = "Scale "   + sp.firmware_latest + " available — tap to install";
    }
    if (text != _lastText) {
        ui_set_ota_banner(true, text.c_str());
        _lastText = text;
    }
}

// Banner-tap handler — fires on either device that has a pending update.
// Sets g_pendingOta for the console and forwards RunOtaUpdate to the scale.
// Both can run together: the scale's update runs in its own task; the
// console reboots after its own otaRun returns.
static void _onOtaBannerTap() {
    auto cp = g_ota_checker.pending();
    const auto& sp = g_scale.scaleOtaPending();
    if (sp.valid && (sp.firmware_update || sp.frontend_update)) {
        g_scale.requestScaleOtaUpdate();
    }
    if (cp.firmware || cp.frontend) {
        g_pendingOta = true;
    }
}

// ── Loop ─────────────────────────────────────────────────────
#define LAT_STEP(name, expr) do {                                        \
    uint32_t __t0 = millis();                                            \
    expr;                                                                \
    uint32_t __dt = millis() - __t0;                                     \
    if (__dt > 50) Serial.printf("[LoopLat] %s=%lums\n", name,           \
                                 (unsigned long)__dt);                   \
} while (0)
void loop() {
    uint32_t __loop_t0 = millis();
    LAT_STEP("wifi",            g_wifi.update());
    LAT_STEP("scale",           g_scale.update());
    LAT_STEP("nfc",             g_nfc.update());
    LAT_STEP("sd",              g_sd.update());
    LAT_STEP("bambu",           g_bambu.update());
    LAT_STEP("bambu_discovery", g_bambu_discovery.update());
    LAT_STEP("scale_discovery", g_scale_discovery.update());
    LAT_STEP("ota_check",       g_ota_checker.update());

    // Once per second, rebuild the home-screen AMS tile strip from whichever
    // printer is currently connected. Cheap in absolute terms but not so
    // cheap it should run at loop() frequency (LVGL lock, string formatting,
    // spool-store lookups × 5). Footer (hostname + IP) rides the same tick.
    static uint32_t _lastAmsRefresh = 0;
    if (millis() - _lastAmsRefresh > 1000) {
        _lastAmsRefresh = millis();
        LAT_STEP("ams_panel", _refreshHomeAmsPanel());
        LAT_STEP("home_foot", _refreshHomeFooter());
        LAT_STEP("ota_banner", _refreshOtaBanner());
    }
    uint32_t __loop_dt = millis() - __loop_t0;
    if (__loop_dt > 100) Serial.printf("[LoopLat] total=%lums\n",
                                       (unsigned long)__loop_dt);

    // Keep the scale-weight cache fresh while the spool-detail screen is
    // open. The scale only pushes LoadChanged* on state transitions; if the
    // load was stable before the console booted / reconnected, the console's
    // `_lastWeightG` stays at 0 until the user wiggles the spool. Polling
    // GetCurrentWeight at ~1 Hz while the user is waiting to capture means
    // the live readout (and the button-press capture helper) always see a
    // recent value. Idle-cheap when no spool screen is up — the whole block
    // skips.
    static uint32_t _lastWeightPollMs = 0;
    if (!g_activeSpoolId.isEmpty() && g_scale.isConnected() &&
        (millis() - _lastWeightPollMs) > 1000) {
        g_scale.requestCurrentWeight();
        _lastWeightPollMs = millis();
    }

    // Deferred auto-close of the spool-detail screen once a scanned spool
    // has been confirmed in an AMS tray. Scheduled by the BambuPrinter
    // onSpoolAssigned hook — dwell gives the user a beat to see the
    // "✓ AMS → <printer> slot N" confirmation before we jump to home.
    if (g_spoolAutoCloseAt && (int32_t)(millis() - g_spoolAutoCloseAt) >= 0) {
        g_spoolAutoCloseAt = 0;
        g_activeSpoolId = "";
        g_activeSpoolExpiresAt = 0;
        ui_show_home();
    }

    // AMS-load timeout: if we armed PendingAms for a scanned spool but the
    // Bambu printer never reported a matching tray load within the 2-minute
    // window, the spool-detail screen has been sitting open waiting. Drop
    // it now — the user's returned the spool to storage or stepped away.
    if (!g_activeSpoolId.isEmpty() &&
        g_spoolAutoCloseAt == 0 &&
        (int32_t)(millis() - g_activeSpoolExpiresAt) >= 0) {
        Serial.printf("[Spool] AMS load timeout for %s — closing screen\n",
                      g_activeSpoolId.c_str());
        ui_set_spool_ams_status(LV_SYMBOL_CLOSE "  AMS: timed out");
        g_activeSpoolId = "";
        g_activeSpoolExpiresAt = 0;
        // Brief dwell so the user sees the "timed out" text before the
        // screen dismisses — reuses the auto-close pipeline.
        g_spoolAutoCloseAt = millis() + SPOOL_AUTOCLOSE_MS;
    }

    // Switch from onboarding → home as soon as WiFi transitions to Connected.
    // Footer labels (IP + hostname) are refreshed by _refreshHomeFooter()
    // on every tick, which covers both this transition and the much more
    // common already-connected-at-boot path that setup() takes directly.
    if (!g_showedHome && g_wifi.getState() == WifiState::Connected) {
        ui_show_home();
        g_showedHome = true;
    }

    // Pending OTA triggered via web API.
    if (g_pendingOta) {
        g_pendingOta = false;
        OtaConfig cfg; cfg.load();
        // Seed the in-flight tracker so /api/ota-status reflects the
        // "preparing…" phase before otaRun's first progress tick lands.
        g_ota_in_flight = { true, "firmware", 0, millis() };
        ui_show_ota_progress(0, "Updating Firmware", "firmware");
        otaRun(cfg, [](OtaProgress p) {
            const char* kind =
                p.kind == OtaProgress::Kind::Frontend ? "frontend" :
                p.kind == OtaProgress::Kind::Firmware ? "firmware" :
                                                        "";
            g_ota_in_flight = { true, kind, p.percent, g_ota_in_flight.started_ms };
            const char* title =
                p.kind == OtaProgress::Kind::Frontend ? "Updating Frontend" :
                p.kind == OtaProgress::Kind::Firmware ? "Updating Firmware" :
                                                        nullptr;
            ui_show_ota_progress(p.percent, title, nullptr);
        });
        // otaRun() reboots on success; if we return, it failed.
        g_ota_in_flight = {};
        ui_show_home();
    }

    delay(5);
}
