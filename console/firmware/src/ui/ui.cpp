#include "ui.h"
#include "config.h"
#include "fonts/spoolhard_fonts.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <lvgl.h>

// ── Theme tokens (mirror shared/frontend/src/app.css) ────────
#define COL_BODY        lv_color_hex(0x0f1117)
#define COL_CARD        lv_color_hex(0x1a1d27)
#define COL_INPUT       lv_color_hex(0x12141c)
#define COL_BORDER      lv_color_hex(0x252830)
#define COL_BRAND       lv_color_hex(0xf0b429)
#define COL_TEXT        lv_color_hex(0xe2e8f0)
#define COL_TEXT_MUTED  lv_color_hex(0x64748b)
#define COL_CONNECTED   lv_color_hex(0x2dd4bf)
#define COL_DISCONN     lv_color_hex(0xf87171)

static lv_obj_t* s_splash    = nullptr;
static lv_obj_t* s_onboard   = nullptr;
static lv_obj_t* s_home      = nullptr;
static lv_obj_t* s_ota       = nullptr;
static lv_obj_t* s_spool     = nullptr;
static lv_obj_t* s_slot      = nullptr;   // AMS-slot detail screen (tap-to-view)

// Spool-screen widget handles. Populated from ui_show_spool().
static lv_obj_t* s_lbl_spool_title     = nullptr;
static lv_obj_t* s_lbl_spool_subtitle  = nullptr;
static lv_obj_t* s_spool_swatch        = nullptr;
static lv_obj_t* s_lbl_spool_current   = nullptr;
static lv_obj_t* s_lbl_spool_advert    = nullptr;
static lv_obj_t* s_lbl_spool_core      = nullptr;
static lv_obj_t* s_lbl_spool_new       = nullptr;
static lv_obj_t* s_lbl_spool_since     = nullptr;
static lv_obj_t* s_lbl_spool_scale     = nullptr;   // live "scale: 1234g stable"
static lv_obj_t* s_lbl_spool_ams       = nullptr;   // AMS auto-assign status
static lv_obj_t* s_btn_spool_current   = nullptr;
static lv_obj_t* s_btn_spool_empty     = nullptr;

// Buffered spool id the screen is currently showing. Forwarded to the
// callback so main.cpp knows which record to mutate.
static char      s_spool_id[40]   = {0};
static spool_cb_t s_spool_cb      = nullptr;

static lv_obj_t* s_lbl_ssid       = nullptr;
static lv_obj_t* s_lbl_key        = nullptr;
static lv_obj_t* s_lbl_onboard_ip = nullptr;  // onboarding screen (portal hint)
static lv_obj_t* s_lbl_weight     = nullptr;
static lv_obj_t* s_lbl_state      = nullptr;
static lv_obj_t* s_led_scale      = nullptr;
static lv_obj_t* s_lbl_scale_text = nullptr;
static lv_obj_t* s_lbl_ip         = nullptr;

// OTA "update available" banner: a thin strip across the bottom of the
// home screen. Hidden by default; populated + shown by ui_set_ota_banner().
// Tapping fires `s_ota_banner_cb` (registered by main.cpp, which kicks the
// actual update). The container is what receives clicks; the inner label
// only carries the text.
static lv_obj_t*       s_ota_banner       = nullptr;
static lv_obj_t*       s_ota_banner_label = nullptr;
static ui_ota_tap_cb_t s_ota_banner_cb    = nullptr;
// Hostname line is split in two so the WiFi glyph can be coloured by link
// state (green/yellow/orange/grey) while the name itself stays white.
static lv_obj_t* s_lbl_wifi_icon  = nullptr;   // coloured WiFi symbol
static lv_obj_t* s_lbl_hostname   = nullptr;   // e.g. "spoolhard-console.local"

// Printer status card (below the weight row). Exposed via
// ui_set_printer_panel() — null snapshot hides the card.
static lv_obj_t* s_printer_card       = nullptr;
static lv_obj_t* s_led_printer        = nullptr;   // connection dot
static lv_obj_t* s_lbl_printer_status = nullptr;   // "name · STATE · N 210°C · B 60°C"
static lv_obj_t* s_bar_printer_prog   = nullptr;   // progress bar
static lv_obj_t* s_lbl_printer_detail = nullptr;   // "42% · layer 120/250"
static lv_obj_t* s_bar_ota        = nullptr;
static lv_obj_t* s_lbl_ota_pct    = nullptr;
static lv_obj_t* s_lbl_ota_kind   = nullptr;
static lv_obj_t* s_lbl_ota_title  = nullptr;

// Registered tap handler for the home-screen AMS tiles. When the user taps
// a tile, we fire this callback with the slot index — main.cpp resolves
// current state + mapped SpoolRecord and calls ui_show_slot_detail().
static ui_slot_tap_cb_t s_slot_tap_cb = nullptr;

// Slot-detail screen widgets. Built once in ui_init(); populated on demand
// from ui_show_slot_detail().
static lv_obj_t* s_slot_title         = nullptr;
static lv_obj_t* s_slot_subtitle      = nullptr;
static lv_obj_t* s_slot_swatch_big    = nullptr;
static lv_obj_t* s_slot_weight_hero   = nullptr;
static lv_obj_t* s_slot_weight_sub    = nullptr;
static lv_obj_t* s_slot_grid          = nullptr;
static lv_obj_t* s_slot_note          = nullptr;

// Home-screen AMS panel — a strip of fixed tiles below the weight card. See
// ui_set_ams_panel() / UiAmsSlot in ui.h.
#define AMS_TILE_COUNT 5
static lv_obj_t* s_ams_panel       = nullptr;
static lv_obj_t* s_lbl_ams_status  = nullptr;
static lv_obj_t* s_ams_tile[AMS_TILE_COUNT]           = {nullptr};
static lv_obj_t* s_ams_swatch[AMS_TILE_COUNT]         = {nullptr};
static lv_obj_t* s_ams_lbl_tag[AMS_TILE_COUNT]        = {nullptr};
static lv_obj_t* s_ams_lbl_material[AMS_TILE_COUNT]   = {nullptr};
static lv_obj_t* s_ams_lbl_weight[AMS_TILE_COUNT]     = {nullptr};
static lv_obj_t* s_ams_lbl_k[AMS_TILE_COUNT]          = {nullptr};

static void style_screen(lv_obj_t* s) {
    lv_obj_set_style_bg_color(s, COL_BODY, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, COL_TEXT, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* make_card(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

// ── Screen builders ─────────────────────────────────────────

static void build_splash() {
    s_splash = lv_obj_create(nullptr);
    style_screen(s_splash);
    lv_obj_t* title = make_label(s_splash, "SpoolHard Console",
                                 COL_BRAND, &spoolhard_mont_36);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);
    lv_obj_t* ver = make_label(s_splash, FW_VERSION, COL_TEXT_MUTED, &spoolhard_mont_18);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 30);
}

static void build_onboarding() {
    s_onboard = lv_obj_create(nullptr);
    style_screen(s_onboard);

    lv_obj_t* title = make_label(s_onboard, "Setup",
                                 COL_BRAND, &spoolhard_mont_28);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t* instr = make_label(s_onboard,
        "Connect to the WiFi below and visit the IP\nto complete setup.",
        COL_TEXT, &spoolhard_mont_16);
    lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t* card = make_card(s_onboard, 40, 110, 400, 170);

    make_label(card, "AP SSID", COL_TEXT_MUTED, &spoolhard_mont_14);
    s_lbl_ssid = make_label(card, "—", COL_BRAND, &spoolhard_mont_22);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t* keyL = make_label(card, "Security key", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(keyL, LV_ALIGN_TOP_LEFT, 0, 70);
    s_lbl_key = make_label(card, "—", COL_BRAND, &spoolhard_mont_22);
    lv_obj_align(s_lbl_key, LV_ALIGN_TOP_LEFT, 0, 90);

    lv_obj_t* ipL = make_label(card, "Portal", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(ipL, LV_ALIGN_TOP_RIGHT, 0, 70);
    s_lbl_onboard_ip = make_label(card, "192.168.4.1", COL_TEXT, &spoolhard_mont_18);
    lv_obj_align(s_lbl_onboard_ip, LV_ALIGN_TOP_RIGHT, 0, 90);
}

static void build_home() {
    s_home = lv_obj_create(nullptr);
    style_screen(s_home);

    // ── Top row (y=12..68, h=56): compact weight + scale ──────────
    // The weight card used to be a hero-number block taking most of the
    // vertical space; it's now a one-line "### g  (state)" strip so the
    // reclaimed rows go to the printer card and a taller AMS panel.
    lv_obj_t* weightCard = make_card(s_home, 12, 12, 280, 56);
    lv_obj_set_style_pad_all(weightCard, 8, 0);
    make_label(weightCard, "Weight", COL_TEXT_MUTED, &spoolhard_mont_14);
    s_lbl_weight = make_label(weightCard, "— g", COL_BRAND, &spoolhard_mont_28);
    lv_obj_align(s_lbl_weight, LV_ALIGN_LEFT_MID, 60, 4);
    s_lbl_state = make_label(weightCard, "waiting", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_lbl_state, LV_ALIGN_RIGHT_MID, -4, 4);

    lv_obj_t* scaleCard = make_card(s_home, 296, 12, 172, 56);
    lv_obj_set_style_pad_all(scaleCard, 8, 0);
    s_led_scale = lv_led_create(scaleCard);
    lv_led_set_color(s_led_scale, COL_DISCONN);
    lv_obj_set_size(s_led_scale, 14, 14);
    lv_obj_align(s_led_scale, LV_ALIGN_LEFT_MID, 0, 0);
    lv_led_off(s_led_scale);
    s_lbl_scale_text = make_label(scaleCard, "Scale", COL_TEXT, &spoolhard_mont_16);
    lv_obj_align(s_lbl_scale_text, LV_ALIGN_LEFT_MID, 24, 0);

    // ── Printer card (y=76..142, h=66): link dot + status line +
    //    progress bar + layer/temp detail. Populated by main.cpp's
    //    periodic refresh via ui_set_printer_panel(). ────────────────
    s_printer_card = make_card(s_home, 12, 76, 456, 66);
    lv_obj_set_style_pad_all(s_printer_card, 8, 0);

    s_led_printer = lv_led_create(s_printer_card);
    lv_led_set_color(s_led_printer, COL_DISCONN);
    lv_obj_set_size(s_led_printer, 12, 12);
    lv_obj_align(s_led_printer, LV_ALIGN_TOP_LEFT, 0, 2);
    lv_led_off(s_led_printer);

    s_lbl_printer_status = make_label(s_printer_card,
        "No printers connected", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_align(s_lbl_printer_status, LV_ALIGN_TOP_LEFT, 20, 0);
    lv_obj_set_width(s_lbl_printer_status, 420);
    lv_label_set_long_mode(s_lbl_printer_status, LV_LABEL_LONG_DOT);

    s_bar_printer_prog = lv_bar_create(s_printer_card);
    lv_obj_set_pos(s_bar_printer_prog, 0, 28);
    lv_obj_set_size(s_bar_printer_prog, 280, 10);
    lv_obj_set_style_bg_color(s_bar_printer_prog, COL_INPUT, 0);
    lv_obj_set_style_bg_color(s_bar_printer_prog, COL_BRAND, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_printer_prog, 4, 0);
    lv_bar_set_range(s_bar_printer_prog, 0, 100);
    lv_bar_set_value(s_bar_printer_prog, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_bar_printer_prog, LV_OBJ_FLAG_HIDDEN);

    s_lbl_printer_detail = make_label(s_printer_card, "",
                                      COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_set_pos(s_lbl_printer_detail, 288, 26);
    lv_obj_set_width(s_lbl_printer_detail, 152);
    lv_label_set_long_mode(s_lbl_printer_detail, LV_LABEL_LONG_DOT);

    // ── AMS panel (y=150..282, h=132): bigger tiles, no header —
    //    the printer card above already carries printer/status. ─────
    s_ams_panel = make_card(s_home, 12, 150, 456, 132);
    lv_obj_set_style_pad_all(s_ams_panel, 6, 0);
    lv_obj_add_flag(s_ams_panel, LV_OBJ_FLAG_HIDDEN);

    // Kept as a hidden placeholder so existing status-setter calls don't
    // crash. If we ever need a transient banner inside the AMS panel, we
    // can un-hide and reposition this one label without another API change.
    s_lbl_ams_status = make_label(s_ams_panel, "", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_add_flag(s_lbl_ams_status, LV_OBJ_FLAG_HIDDEN);

    // Tile strip — five fixed slots (AMS 1.1..1.4 + Ext), now 87×120:
    // a tall swatch, a big material/weight pair, and K at the bottom.
    const int tileW    = 87;
    const int tileH    = 120;
    const int tileGapX = 2;
    const int tileY    = 0;
    int       x        = 0;
    for (int i = 0; i < AMS_TILE_COUNT; ++i) {
        lv_obj_t* tile = lv_obj_create(s_ams_panel);
        lv_obj_set_pos(tile, x, tileY);
        lv_obj_set_size(tile, tileW, tileH);
        lv_obj_set_style_bg_color(tile, COL_INPUT, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, COL_BORDER, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, 6, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        // Tap-to-open the slot detail screen. The slot index rides along as
        // user_data; the handler extracts it and forwards to the registered
        // C callback owned by main.cpp (resolves state + SpoolRecord there).
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (s_slot_tap_cb) s_slot_tap_cb(idx);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_ams_tile[i] = tile;

        // Colour swatch strip (top). 36px tall — the dominant visual.
        lv_obj_t* sw = lv_obj_create(tile);
        lv_obj_set_pos(sw, 0, 0);
        lv_obj_set_size(sw, tileW, 36);
        lv_obj_set_style_bg_color(sw, COL_INPUT, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_set_style_radius(sw, 0, 0);
        lv_obj_set_style_pad_all(sw, 0, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        // Let clicks on the swatch bubble up to the tile so the entire
        // card is tappable, not just the body below the swatch.
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        s_ams_swatch[i] = sw;

        s_ams_lbl_tag[i] = make_label(sw, "—", COL_TEXT_MUTED, &spoolhard_mont_14);
        lv_obj_align(s_ams_lbl_tag[i], LV_ALIGN_LEFT_MID, 4, 0);

        // Body — bigger fonts: material 18pt, weight 22pt hero, K 14pt.
        s_ams_lbl_material[i] = make_label(tile, "", COL_TEXT, &spoolhard_mont_18);
        lv_obj_align(s_ams_lbl_material[i], LV_ALIGN_TOP_LEFT, 6, 40);
        s_ams_lbl_weight[i]   = make_label(tile, "", COL_BRAND, &spoolhard_mont_22);
        lv_obj_align(s_ams_lbl_weight[i], LV_ALIGN_TOP_LEFT, 6, 62);
        s_ams_lbl_k[i]        = make_label(tile, "", COL_TEXT_MUTED, &spoolhard_mont_14);
        lv_obj_align(s_ams_lbl_k[i], LV_ALIGN_TOP_LEFT, 6, 96);

        x += tileW + tileGapX;
    }

    // ── Footer row: hostname (left) / version (centre) / IP (right) ─
    // Hostname and IP are driven by the periodic WiFi state — mDNS and
    // DHCP respectively — via ui_set_hostname() / ui_set_ip(). Version is
    // static so we bake it straight in here.
    //
    // The hostname line is actually two labels: a coloured WiFi glyph
    // whose tint reflects link state (green/yellow/orange/grey), and the
    // hostname text in plain white right after it.
    s_lbl_wifi_icon = make_label(s_home, LV_SYMBOL_WIFI,
                                 COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_lbl_wifi_icon, LV_ALIGN_BOTTOM_LEFT, 12, -6);
    s_lbl_hostname = make_label(s_home, "—",
                                COL_TEXT, &spoolhard_mont_14);
    lv_obj_align_to(s_lbl_hostname, s_lbl_wifi_icon,
                    LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t* foot = make_label(s_home, "V " FW_VERSION,
                                COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_lbl_ip = make_label(s_home, "—", COL_TEXT, &spoolhard_mont_14);
    lv_obj_align(s_lbl_ip, LV_ALIGN_BOTTOM_RIGHT, -12, -6);

    // OTA banner — hidden until main.cpp asks us to show it. Sits across
    // the bottom of the screen, OVER the footer text (which becomes
    // less important the moment there's an update to apply). Brand-amber
    // background so the user can't miss it from across the room.
    s_ota_banner = lv_obj_create(s_home);
    lv_obj_set_pos(s_ota_banner, 0, 286);
    lv_obj_set_size(s_ota_banner, 480, 34);
    lv_obj_set_style_radius(s_ota_banner, 0, 0);
    lv_obj_set_style_bg_color(s_ota_banner, COL_BRAND, 0);
    lv_obj_set_style_bg_opa(s_ota_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ota_banner, 0, 0);
    lv_obj_set_style_pad_all(s_ota_banner, 0, 0);
    lv_obj_add_flag(s_ota_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ota_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ota_banner, [](lv_event_t*) {
        if (s_ota_banner_cb) s_ota_banner_cb();
    }, LV_EVENT_CLICKED, nullptr);

    s_ota_banner_label = make_label(s_ota_banner, "", COL_BODY, &spoolhard_mont_16);
    lv_obj_center(s_ota_banner_label);
}

static void on_spool_btn(lv_event_t* e) {
    // lv_event_get_user_data() carries the button's action code as a void*.
    spool_btn_t action = (spool_btn_t)(intptr_t)lv_event_get_user_data(e);
    Serial.printf("[UI] spool button click action=%d id='%s'\n", (int)action, s_spool_id);
    if (s_spool_cb) s_spool_cb(action, s_spool_id);
    else            Serial.println("[UI] spool button: no callback registered");
}

// Helper: make a primary (brand) or secondary (surface) button with a label.
static lv_obj_t* make_spool_button(lv_obj_t* parent, const char* text, bool primary,
                                   spool_btn_t action, int x, int y, int w) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 44);
    lv_obj_set_style_radius(b, 8, 0);
    // Primary: amber brand colour. Secondary: COL_CARD bg + COL_TEXT_MUTED
    // border — COL_INPUT was only one luminance step above COL_BODY, so
    // secondary buttons visually disappeared into the screen background.
    lv_obj_set_style_bg_color(b, primary ? COL_BRAND : COL_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, primary ? COL_BORDER : COL_TEXT_MUTED, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_add_event_cb(b, on_spool_btn, LV_EVENT_CLICKED,
                        (void*)(intptr_t)action);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, primary ? COL_BODY : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &spoolhard_mont_16, 0);
    lv_obj_center(l);
    return b;
}

static void build_spool() {
    s_spool = lv_obj_create(nullptr);
    style_screen(s_spool);

    // Header: color swatch + title + subtitle
    s_spool_swatch = lv_obj_create(s_spool);
    lv_obj_set_pos(s_spool_swatch, 12, 14);
    lv_obj_set_size(s_spool_swatch, 32, 32);
    lv_obj_set_style_radius(s_spool_swatch, 16, 0);
    lv_obj_set_style_border_color(s_spool_swatch, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_spool_swatch, 1, 0);
    lv_obj_set_style_bg_color(s_spool_swatch, COL_INPUT, 0);
    lv_obj_set_style_bg_opa(s_spool_swatch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_spool_swatch, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_spool_title = make_label(s_spool, "Spool", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(s_lbl_spool_title, 54, 12);
    s_lbl_spool_subtitle = make_label(s_spool, "", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_set_pos(s_lbl_spool_subtitle, 54, 36);
    lv_obj_set_width(s_lbl_spool_subtitle, 414);
    lv_label_set_long_mode(s_lbl_spool_subtitle, LV_LABEL_LONG_DOT);

    // Weight card (left): big current weight + small trio
    lv_obj_t* wCard = make_card(s_spool, 12, 58, 280, 140);
    make_label(wCard, "Current", COL_TEXT_MUTED, &spoolhard_mont_14);
    s_lbl_spool_current = make_label(wCard, "— g", COL_BRAND, &spoolhard_mont_36);
    lv_obj_align(s_lbl_spool_current, LV_ALIGN_CENTER, 0, -8);
    s_lbl_spool_since = make_label(wCard, "", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_lbl_spool_since, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Detail card (right): advertised / core / new
    lv_obj_t* dCard = make_card(s_spool, 300, 58, 168, 140);
    make_label(dCard, "Advertised",   COL_TEXT_MUTED, &spoolhard_mont_14);
    s_lbl_spool_advert = make_label(dCard, "—", COL_TEXT, &spoolhard_mont_16);
    lv_obj_align(s_lbl_spool_advert, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t* lblCore = make_label(dCard, "Empty core", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(lblCore, LV_ALIGN_TOP_LEFT, 0, 32);
    s_lbl_spool_core = make_label(dCard, "—", COL_TEXT, &spoolhard_mont_16);
    lv_obj_align(s_lbl_spool_core, LV_ALIGN_TOP_RIGHT, 0, 32);

    lv_obj_t* lblNew = make_label(dCard, "New",         COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(lblNew, LV_ALIGN_TOP_LEFT, 0, 64);
    s_lbl_spool_new = make_label(dCard, "—", COL_TEXT, &spoolhard_mont_16);
    lv_obj_align(s_lbl_spool_new, LV_ALIGN_TOP_RIGHT, 0, 64);

    // Live scale readout strip
    s_lbl_spool_scale = make_label(s_spool, "Scale: --", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_set_pos(s_lbl_spool_scale, 12, 206);

    // AMS auto-assignment status (right of the scale label). Hidden until
    // the PendingAms latch arms it on a tag scan.
    s_lbl_spool_ams = make_label(s_spool, "", COL_BRAND, &spoolhard_mont_14);
    lv_obj_set_pos(s_lbl_spool_ams, 220, 206);
    lv_obj_set_width(s_lbl_spool_ams, 248);
    lv_label_set_long_mode(s_lbl_spool_ams, LV_LABEL_LONG_DOT);

    // Action row
    s_btn_spool_current = make_spool_button(s_spool, "Capture weight", true,
                                            SPOOL_BTN_CAPTURE_CURRENT, 12, 236, 200);
    s_btn_spool_empty   = make_spool_button(s_spool, "Capture empty",  false,
                                            SPOOL_BTN_CAPTURE_EMPTY,  220, 236, 160);
    make_spool_button(s_spool, "Close", false, SPOOL_BTN_CLOSE, 388, 236, 80);
}

static void build_ota() {
    s_ota = lv_obj_create(nullptr);
    style_screen(s_ota);

    s_lbl_ota_title = make_label(s_ota, "Updating",
                                 COL_BRAND, &spoolhard_mont_28);
    lv_obj_align(s_lbl_ota_title, LV_ALIGN_CENTER, 0, -70);

    s_lbl_ota_kind = make_label(s_ota, "firmware", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_align(s_lbl_ota_kind, LV_ALIGN_CENTER, 0, -40);

    s_bar_ota = lv_bar_create(s_ota);
    lv_obj_set_size(s_bar_ota, 360, 18);
    lv_obj_align(s_bar_ota, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_bar_ota, COL_INPUT, 0);
    lv_obj_set_style_bg_color(s_bar_ota, COL_BRAND, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar_ota, 0, 100);
    lv_bar_set_value(s_bar_ota, 0, LV_ANIM_OFF);

    s_lbl_ota_pct = make_label(s_ota, "0%", COL_TEXT, &spoolhard_mont_22);
    lv_obj_align(s_lbl_ota_pct, LV_ALIGN_CENTER, 0, 40);
}

// Full slot-detail screen — reached by tapping a tile on the home AMS
// strip. Layout (480×320):
//   Header  : title + close button
//   Left    : large color swatch + weight hero
//   Right   : two-column grid of everything we know (AMS + SpoolRecord)
//   Bottom  : note line (truncated if long)
static void build_slot_detail() {
    s_slot = lv_obj_create(nullptr);
    style_screen(s_slot);

    // Header
    s_slot_title = make_label(s_slot, "Slot", COL_BRAND, &spoolhard_mont_22);
    lv_obj_align(s_slot_title, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_set_width(s_slot_title, 380);
    lv_label_set_long_mode(s_slot_title, LV_LABEL_LONG_DOT);

    s_slot_subtitle = make_label(s_slot, "", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_slot_subtitle, LV_ALIGN_TOP_LEFT, 12, 38);
    lv_obj_set_width(s_slot_subtitle, 380);
    lv_label_set_long_mode(s_slot_subtitle, LV_LABEL_LONG_DOT);

    // Close button (top-right). COL_CARD bg + COL_TEXT_MUTED border so it
    // stands out against the screen's darker body colour — COL_INPUT looked
    // dark-on-dark from a normal viewing distance.
    lv_obj_t* closeBtn = lv_btn_create(s_slot);
    lv_obj_set_size(closeBtn, 48, 34);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_set_style_bg_color(closeBtn, COL_CARD, 0);
    lv_obj_set_style_border_color(closeBtn, COL_TEXT_MUTED, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t*) {
        ui_show_home();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* closeLbl = lv_label_create(closeBtn);
    lv_label_set_text(closeLbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(closeLbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(closeLbl, &spoolhard_mont_16, 0);
    lv_obj_center(closeLbl);

    // Left column: large color swatch + weight hero below.
    s_slot_swatch_big = lv_obj_create(s_slot);
    lv_obj_set_pos(s_slot_swatch_big, 12, 68);
    lv_obj_set_size(s_slot_swatch_big, 140, 100);
    lv_obj_set_style_bg_color(s_slot_swatch_big, COL_INPUT, 0);
    lv_obj_set_style_bg_opa(s_slot_swatch_big, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_slot_swatch_big, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_slot_swatch_big, 1, 0);
    lv_obj_set_style_radius(s_slot_swatch_big, 8, 0);
    lv_obj_clear_flag(s_slot_swatch_big, LV_OBJ_FLAG_SCROLLABLE);

    s_slot_weight_hero = make_label(s_slot, "—",
                                    COL_BRAND, &spoolhard_mont_36);
    lv_obj_align(s_slot_weight_hero, LV_ALIGN_TOP_LEFT, 12, 178);
    s_slot_weight_sub = make_label(s_slot, "",
                                   COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_slot_weight_sub, LV_ALIGN_TOP_LEFT, 12, 224);
    lv_obj_set_width(s_slot_weight_sub, 140);
    lv_label_set_long_mode(s_slot_weight_sub, LV_LABEL_LONG_WRAP);

    // Right column: a scrollable block holding the detail lines. Populated
    // fresh each time the screen opens — we clear + rebuild rather than
    // trying to keep a fixed set of labels in sync.
    s_slot_grid = lv_obj_create(s_slot);
    lv_obj_set_pos(s_slot_grid, 164, 68);
    lv_obj_set_size(s_slot_grid, 304, 210);
    lv_obj_set_style_bg_color(s_slot_grid, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_slot_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_slot_grid, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_slot_grid, 1, 0);
    lv_obj_set_style_radius(s_slot_grid, 8, 0);
    lv_obj_set_style_pad_all(s_slot_grid, 10, 0);
    lv_obj_set_flex_flow(s_slot_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_slot_grid, 4, 0);

    // Note line at the very bottom, spans full width.
    s_slot_note = make_label(s_slot, "", COL_TEXT_MUTED, &spoolhard_mont_14);
    lv_obj_align(s_slot_note, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_width(s_slot_note, 456);
    lv_label_set_long_mode(s_slot_note, LV_LABEL_LONG_DOT);
}

// ── Public API ──────────────────────────────────────────────

void ui_init() {
    lv_lock();
    build_splash();
    build_onboarding();
    build_home();
    build_ota();
    build_spool();
    build_slot_detail();
    lv_unlock();
}

void ui_show_splash()     { lv_lock(); lv_screen_load(s_splash);   lv_unlock(); }
void ui_show_onboarding() { lv_lock(); lv_screen_load(s_onboard);  lv_unlock(); }
void ui_show_home()       { lv_lock(); lv_screen_load(s_home);     lv_unlock(); }

void ui_set_ota_banner(bool show, const char* text) {
    if (!s_ota_banner) return;  // build_home hasn't run yet (boot race)
    lv_lock();
    if (show) {
        lv_label_set_text(s_ota_banner_label, text ? text : "Update available — tap to install");
        lv_obj_clear_flag(s_ota_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ota_banner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_unlock();
}

void ui_set_ota_banner_callback(ui_ota_tap_cb_t cb) { s_ota_banner_cb = cb; }

void ui_set_scale_state(scale_lcd_state_t state, const char* scale_name) {
    lv_lock();
    // The LED is the only status signal: red=missing, orange=discovering,
    // yellow=unencrypted, green=encrypted. Text carries just the name so
    // the user isn't reading four different suffixes to work out what's
    // going on.
    lv_color_t col;
    switch (state) {
        case SCALE_LCD_ENCRYPTED:    col = lv_color_hex(0x22c55e); break; // green
        case SCALE_LCD_UNENCRYPTED:  col = lv_color_hex(0xfacc15); break; // yellow
        case SCALE_LCD_DISCOVERING:  col = lv_color_hex(0xfb923c); break; // orange
        default: /* MISSING */       col = lv_color_hex(0xf87171); break; // red
    }
    if (s_led_scale) {
        lv_led_set_color(s_led_scale, col);
        lv_led_on(s_led_scale);   // always on — the colour is the signal
    }
    if (s_lbl_scale_text) {
        const bool has_name = scale_name && *scale_name;
        lv_label_set_long_mode(s_lbl_scale_text, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_lbl_scale_text, 118);
        lv_label_set_text(s_lbl_scale_text, has_name ? scale_name : "No scale");
    }
    lv_unlock();
}

void ui_set_spool_callback(spool_cb_t cb) { s_spool_cb = cb; }

// Small helper — formats "1234 g" into buf when v >= 0, or "—" otherwise.
static void _fmt_g(char* buf, size_t n, int v) {
    if (v < 0) { snprintf(buf, n, "—"); return; }
    snprintf(buf, n, "%d g", v);
}

void ui_show_spool(const char* spool_id,
                   const char* title,
                   const char* subtitle,
                   const char* color_hex,
                   int  weight_current,
                   int  weight_advertised,
                   int  weight_core,
                   int  weight_new,
                   int  consumed_since_weight) {
    lv_lock();
    // Cache the id so button events carry it through.
    snprintf(s_spool_id, sizeof(s_spool_id), "%s", spool_id ? spool_id : "");

    lv_label_set_text(s_lbl_spool_title,    title    ? title    : "Spool");
    lv_label_set_text(s_lbl_spool_subtitle, subtitle ? subtitle : "");

    // Parse the hex color (RRGGBB or RRGGBBAA) for the swatch. Invalid or
    // empty → leave it at the placeholder input color.
    if (color_hex && strlen(color_hex) >= 6) {
        char tmp[7]; memcpy(tmp, color_hex, 6); tmp[6] = 0;
        uint32_t rgb = strtoul(tmp, nullptr, 16);
        lv_obj_set_style_bg_color(s_spool_swatch, lv_color_hex(rgb), 0);
    } else {
        lv_obj_set_style_bg_color(s_spool_swatch, COL_INPUT, 0);
    }

    char buf[32];
    _fmt_g(buf, sizeof(buf), weight_current);
    lv_label_set_text(s_lbl_spool_current, buf);

    _fmt_g(buf, sizeof(buf), weight_advertised);
    lv_label_set_text(s_lbl_spool_advert, buf);
    _fmt_g(buf, sizeof(buf), weight_core);
    lv_label_set_text(s_lbl_spool_core, buf);
    _fmt_g(buf, sizeof(buf), weight_new);
    lv_label_set_text(s_lbl_spool_new, buf);

    if (consumed_since_weight > 0) {
        snprintf(buf, sizeof(buf), "- %d g since last weigh", consumed_since_weight);
        lv_label_set_text(s_lbl_spool_since, buf);
    } else {
        lv_label_set_text(s_lbl_spool_since, "");
    }

    if (s_lbl_spool_ams) lv_label_set_text(s_lbl_spool_ams, "");

    lv_screen_load(s_spool);
    lv_unlock();
}

void ui_set_spool_ams_status(const char* text) {
    if (!s_lbl_spool_ams) return;
    lv_lock();
    lv_label_set_text(s_lbl_spool_ams, text ? text : "");
    lv_unlock();
}

// Flash the weight-current label on the spool screen to signal a just-
// applied capture. Three on/off cycles of white → COL_BRAND so the user
// visually registers the number changed. lv_timer_t is simpler than
// lv_anim_t here because we only need a discrete toggle, not a property
// interpolation; it also naturally self-cleans when the cycle count hits 0.
static uint8_t      s_spool_flash_count = 0;
static lv_timer_t*  s_spool_flash_timer = nullptr;

static void _spool_flash_step(lv_timer_t* t) {
    if (!s_lbl_spool_current) {
        lv_timer_delete(t);
        s_spool_flash_timer = nullptr;
        s_spool_flash_count = 0;
        return;
    }
    // Even step → hot (white), odd → back to brand. Cycle count is the
    // number of toggles left to play; stops on 0 with the label restored
    // to its normal COL_BRAND so no UI residue leaks out of the flash.
    bool hot = (s_spool_flash_count % 2 == 0);
    lv_obj_set_style_text_color(s_lbl_spool_current,
                                hot ? lv_color_white() : COL_BRAND, 0);
    if (s_spool_flash_count == 0) {
        lv_timer_delete(t);
        s_spool_flash_timer = nullptr;
        return;
    }
    --s_spool_flash_count;
}

void ui_flash_spool_current() {
    if (!s_lbl_spool_current) return;
    lv_lock();
    if (s_spool_flash_timer) {
        lv_timer_delete(s_spool_flash_timer);
        s_spool_flash_timer = nullptr;
    }
    // 6 toggles × 140 ms ≈ 840 ms total — long enough to be obvious, short
    // enough not to delay the user's next action. Starts on `hot` (white).
    s_spool_flash_count = 6;
    s_spool_flash_timer = lv_timer_create(_spool_flash_step, 140, nullptr);
    lv_unlock();
}

// Pick black or white text that reads well over the given RGB background.
// Rec. 601 luma, same heuristic the web frontend uses in PrintersPanel.
static lv_color_t _textColorFor(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8)  & 0xFF;
    uint8_t b =  rgb        & 0xFF;
    uint16_t luma = (299 * r + 587 * g + 114 * b) / 1000;
    return luma > 160 ? lv_color_hex(0x111827) : lv_color_hex(0xf9fafb);
}

void ui_set_printer_panel(const UiPrinterSnapshot* snap) {
    if (!s_printer_card) return;
    lv_lock();

    if (!snap || !snap->connected) {
        // Placeholder: grey LED, muted "no printer" line, hide bar/detail.
        lv_led_set_color(s_led_printer, COL_DISCONN);
        lv_led_off(s_led_printer);
        lv_obj_set_style_text_color(s_lbl_printer_status, COL_TEXT_MUTED, 0);
        lv_label_set_text(s_lbl_printer_status,
                          snap ? "Printer offline" : "No printer");
        lv_obj_add_flag(s_bar_printer_prog, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_lbl_printer_detail, "");
        lv_unlock();
        return;
    }

    lv_led_set_color(s_led_printer, COL_CONNECTED);
    lv_led_on(s_led_printer);

    // Status line: name + state + temps (one row, dot-truncated). Temps
    // appear inline so they're visible even when the progress bar is
    // hidden (IDLE / FINISH). Using LV_SYMBOL_CHARGE for the "running"
    // state would be nice but we only have the FA symbols we compiled in.
    char status[96];
    const bool have_temps = snap->has_temps;
    if (snap->gcode_state[0]) {
        if (have_temps) {
            snprintf(status, sizeof(status),
                     "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET
                     "  N %.0f" "\xC2\xB0" "C  " LV_SYMBOL_BULLET "  B %.0f" "\xC2\xB0" "C",
                     snap->name, snap->gcode_state,
                     snap->nozzle_temp, snap->bed_temp);
        } else {
            snprintf(status, sizeof(status), "%s  " LV_SYMBOL_BULLET "  %s",
                     snap->name, snap->gcode_state);
        }
    } else {
        snprintf(status, sizeof(status), "%s", snap->name);
    }
    lv_obj_set_style_text_color(s_lbl_printer_status, COL_TEXT, 0);
    lv_label_set_text(s_lbl_printer_status, status);

    // Progress bar + layer detail only while there's something to report.
    const bool printing = snap->progress_pct >= 0;
    if (printing) {
        lv_obj_clear_flag(s_bar_printer_prog, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar_printer_prog, snap->progress_pct, LV_ANIM_OFF);
        char det[48];
        if (snap->layer_num >= 0 && snap->total_layers > 0) {
            snprintf(det, sizeof(det), "%d%%  " LV_SYMBOL_BULLET "  layer %d/%d",
                     snap->progress_pct, snap->layer_num, snap->total_layers);
        } else {
            snprintf(det, sizeof(det), "%d%%", snap->progress_pct);
        }
        lv_label_set_text(s_lbl_printer_detail, det);
    } else {
        lv_obj_add_flag(s_bar_printer_prog, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_lbl_printer_detail, "");
    }

    lv_unlock();
}

void ui_set_ams_panel(const char* status, const UiAmsSlot* slots) {
    if (!s_ams_panel) return;

    lv_lock();
    const bool show = status && *status;
    if (!show) {
        lv_obj_add_flag(s_ams_panel, LV_OBJ_FLAG_HIDDEN);
        lv_unlock();
        return;
    }
    lv_obj_clear_flag(s_ams_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_ams_status, status);

    for (int i = 0; i < AMS_TILE_COUNT; ++i) {
        const UiAmsSlot* s = slots ? &slots[i] : nullptr;
        const bool occupied = s && s->occupied;

        // Active tray gets a brand-accent border so the user can see which
        // slot is currently feeding the extruder at a glance.
        lv_obj_set_style_border_color(
            s_ams_tile[i],
            (s && s->active) ? COL_BRAND : COL_BORDER,
            0);
        lv_obj_set_style_border_width(s_ams_tile[i], (s && s->active) ? 2 : 1, 0);

        // Swatch colour + tag text colour.
        uint32_t rgb = occupied ? (s->color_rgb ? s->color_rgb : 0x303540) : 0x1a1d27;
        lv_obj_set_style_bg_color(s_ams_swatch[i], lv_color_hex(rgb), 0);
        lv_color_t tag_col = occupied && s->color_rgb ? _textColorFor(rgb) : COL_TEXT_MUTED;
        lv_obj_set_style_text_color(s_ams_lbl_tag[i], tag_col, 0);
        lv_label_set_text(s_ams_lbl_tag[i], s ? s->label : "");

        // Body.
        lv_label_set_text(s_ams_lbl_material[i], occupied ? s->material : "empty");
        lv_obj_set_style_text_color(s_ams_lbl_material[i],
                                    occupied ? COL_TEXT : COL_TEXT_MUTED, 0);

        char wbuf[16];
        if (occupied && s->weight_g >= 0) {
            snprintf(wbuf, sizeof(wbuf), "%d g", (int)s->weight_g);
        } else if (occupied && s->remain_pct >= 0) {
            snprintf(wbuf, sizeof(wbuf), "~%d%%", s->remain_pct);
        } else {
            snprintf(wbuf, sizeof(wbuf), "—");
        }
        lv_label_set_text(s_ams_lbl_weight[i], wbuf);

        char kbuf[16];
        if (occupied && s->k > 0.f) snprintf(kbuf, sizeof(kbuf), "K %.3f", (double)s->k);
        else                        kbuf[0] = 0;
        lv_label_set_text(s_ams_lbl_k[i], kbuf);
    }
    lv_unlock();
}

void ui_set_spool_live_weight(float grams, const char* state) {
    // Cheap guard: only bother if the spool screen is currently on top.
    if (!s_spool || lv_screen_active() != s_spool) return;
    lv_lock();
    char buf[48];
    if (state && strcmp(state, "removed") == 0) {
        snprintf(buf, sizeof(buf), "Scale: -- (empty)");
    } else if (state && strcmp(state, "uncalibrated") == 0) {
        snprintf(buf, sizeof(buf), "Scale: uncalibrated");
    } else if (state && *state) {
        snprintf(buf, sizeof(buf), "Scale: %.0f g  (%s)", grams, state);
    } else {
        snprintf(buf, sizeof(buf), "Scale: --");
    }
    lv_label_set_text(s_lbl_spool_scale, buf);
    lv_unlock();
}

void ui_show_ota_progress(int percent, const char* title, const char* version) {
    if (percent < 0) percent = 0; if (percent > 100) percent = 100;
    lv_lock();
    lv_screen_load(s_ota);
    if (title)   lv_label_set_text(s_lbl_ota_title, title);
    if (version) lv_label_set_text(s_lbl_ota_kind,  version);
    lv_bar_set_value(s_bar_ota, percent, LV_ANIM_OFF);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(s_lbl_ota_pct, buf);
    lv_unlock();
}

void ui_set_scale_connected(bool connected) {
    lv_lock();
    if (s_led_scale) {
        lv_led_set_color(s_led_scale, connected ? COL_CONNECTED : COL_DISCONN);
        if (connected) lv_led_on(s_led_scale); else lv_led_off(s_led_scale);
    }
    if (s_lbl_scale_text) {
        lv_label_set_text(s_lbl_scale_text, connected ? "Scale online" : "Scale offline");
    }
    lv_unlock();
}

void ui_set_weight(float grams, const char* state, int precision) {
    // Render with the scale's configured decimal count. Clamp to 0..4 so a
    // bogus precision can't overflow wbuf (max "12345.6789 g" = 12 chars).
    if (precision < 0) precision = 0;
    if (precision > 4) precision = 4;
    char wbuf[24];
    snprintf(wbuf, sizeof(wbuf), "%.*f g", precision, grams);
    lv_lock();
    if (s_lbl_weight) lv_label_set_text(s_lbl_weight, wbuf);
    if (s_lbl_state && state) lv_label_set_text(s_lbl_state, state);
    lv_unlock();
}

void ui_set_last_tag(const char* /*uid*/, const char* /*url*/) {
    // No-op — the home screen no longer has a dedicated last-tag card.
    // Kept as a named stub so main.cpp's existing call site doesn't need
    // to change if we re-surface the info somewhere else later.
}

void ui_set_onboarding(const char* ap_ssid, const char* security_key, const char* ip_or_mdns) {
    lv_lock();
    if (s_lbl_ssid       && ap_ssid)       lv_label_set_text(s_lbl_ssid,       ap_ssid);
    if (s_lbl_key        && security_key)  lv_label_set_text(s_lbl_key,        security_key);
    if (s_lbl_onboard_ip && ip_or_mdns)    lv_label_set_text(s_lbl_onboard_ip, ip_or_mdns);
    lv_unlock();
}

void ui_set_ip(const char* ip) {
    if (!ip) return;
    lv_lock();
    if (s_lbl_ip) lv_label_set_text(s_lbl_ip, ip);
    lv_unlock();
}

void ui_set_hostname(const char* hostname, wifi_lcd_state_t state) {
    if (!s_lbl_hostname || !s_lbl_wifi_icon) return;
    lv_lock();
    // Icon colour = WiFi link state. Text stays white regardless so the
    // hostname is always easy to read.
    lv_color_t icon_col = COL_TEXT_MUTED;
    switch (state) {
        case WIFI_LCD_CONNECTED:   icon_col = lv_color_hex(0x22c55e); break; // green
        case WIFI_LCD_CONNECTING:  icon_col = lv_color_hex(0xfacc15); break; // yellow
        case WIFI_LCD_AP:          icon_col = lv_color_hex(0xfb923c); break; // orange
        default: /* DISCONNECTED */ icon_col = COL_TEXT_MUTED;        break;
    }
    lv_obj_set_style_text_color(s_lbl_wifi_icon, icon_col, 0);
    lv_label_set_text(s_lbl_hostname, (hostname && *hostname) ? hostname : "—");
    // Re-align the text after the (fixed-width) icon in case the icon
    // label's width shifted — it won't, but cheap to be defensive.
    lv_obj_align_to(s_lbl_hostname, s_lbl_wifi_icon,
                    LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_unlock();
}

// ── Slot detail screen setters ─────────────────────────────────

void ui_set_slot_tap_callback(ui_slot_tap_cb_t cb) {
    s_slot_tap_cb = cb;
}

// Append a "key: value" row to the detail grid. `muted` draws the label
// grey so the eye skips to values — makes the column scannable.
static void _slot_row(const char* key, const char* value) {
    lv_obj_t* row = lv_obj_create(s_slot_grid);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, COL_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(k, &spoolhard_mont_14, 0);
    lv_obj_set_width(k, 100);

    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, value && *value ? value : "—");
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_set_style_text_font(v, &spoolhard_mont_14, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
}

void ui_show_slot_detail(const UiSlotDetail* d) {
    if (!s_slot || !d) return;
    lv_lock();

    // Header
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s  %s  %s",
             d->printer_name[0] ? d->printer_name : "Printer",
             LV_SYMBOL_BULLET,
             d->slot_label[0] ? d->slot_label : "slot");
    lv_label_set_text(s_slot_title, hdr);

    // Subtitle — the spool's brand/material if we have one mapped, else the
    // printer-reported material (keeps the screen useful for empty slots).
    char sub[80];
    if (d->has_spool) {
        snprintf(sub, sizeof(sub), "%s%s%s%s%s",
                 d->brand[0] ? d->brand : "",
                 d->brand[0] && d->material[0] ? " " LV_SYMBOL_BULLET " " : "",
                 d->material[0] ? d->material : "",
                 d->material_subtype[0] ? " " : "",
                 d->material_subtype);
    } else if (d->material[0]) {
        snprintf(sub, sizeof(sub), "%s (no spool mapped)", d->material);
    } else {
        snprintf(sub, sizeof(sub), "Empty slot");
    }
    lv_label_set_text(s_slot_subtitle, sub);

    // Colour swatch — AMS wins when it reports a non-zero color (ground
    // truth for what's physically in the slot); SpoolRecord is the
    // fallback. Matches the home-tile priority so the two stay consistent.
    uint32_t rgb = 0;
    if (d->color_rgb != 0) {
        rgb = d->color_rgb;
    } else if (d->has_spool && d->color_code[0]) {
        rgb = (uint32_t)strtoul(d->color_code, nullptr, 16);
    }
    if (rgb == 0) {
        // Visible placeholder (the card outline alone gets lost on dark BG).
        lv_obj_set_style_bg_color(s_slot_swatch_big, COL_INPUT, 0);
    } else {
        lv_obj_set_style_bg_color(s_slot_swatch_big, lv_color_hex(rgb), 0);
    }

    // Weight hero — filament-only weight from the spool record. Fall back
    // to remain_pct for a rough indicator when no record is mapped.
    char wbuf[24];
    char wsub[64];
    if (d->has_spool && d->weight_current_g >= 0) {
        snprintf(wbuf, sizeof(wbuf), "%d g", (int)d->weight_current_g);
    } else if (d->remain_pct >= 0) {
        snprintf(wbuf, sizeof(wbuf), "~%d%%", (int)d->remain_pct);
    } else {
        snprintf(wbuf, sizeof(wbuf), "—");
    }
    lv_label_set_text(s_slot_weight_hero, wbuf);

    if (d->has_spool) {
        if (d->consumed_since_weight_g > 0.5f) {
            snprintf(wsub, sizeof(wsub), "-%d g since last weigh",
                     (int)d->consumed_since_weight_g);
        } else if (d->weight_advertised_g > 0) {
            snprintf(wsub, sizeof(wsub), "advertised %d g",
                     (int)d->weight_advertised_g);
        } else {
            wsub[0] = 0;
        }
    } else {
        wsub[0] = 0;
    }
    lv_label_set_text(s_slot_weight_sub, wsub);

    // Rebuild the detail grid from scratch — the set of fields we have
    // worth showing varies per spool, so there's no fixed template.
    lv_obj_clean(s_slot_grid);

    // AMS-reported state (always shown — applies even without a spool)
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s%s",
             d->slot_label, d->active ? "  (active)" : "",
             d->mapped_via_override ? "  (pinned)" : "");
    _slot_row("Slot", buf);

    if (d->k > 0.f) {
        snprintf(buf, sizeof(buf), "%.3f%s%s",
                 (double)d->k,
                 d->cali_idx >= 0 ? "  idx " : "",
                 "");
        if (d->cali_idx >= 0) {
            snprintf(buf, sizeof(buf), "%.3f  idx %d",
                     (double)d->k, (int)d->cali_idx);
        } else {
            snprintf(buf, sizeof(buf), "%.3f", (double)d->k);
        }
        _slot_row("K", buf);
    }

    // Temperature priority: spool record overrides printer report. Show
    // both when they differ so the user spots drift.
    if (d->has_spool && d->nozzle_temp_min > 0 && d->nozzle_temp_max > 0) {
        snprintf(buf, sizeof(buf), "%d – %d °C",
                 (int)d->nozzle_temp_min, (int)d->nozzle_temp_max);
        _slot_row("Nozzle", buf);
    } else if (d->ams_nozzle_min_c > 0 && d->ams_nozzle_max_c > 0) {
        snprintf(buf, sizeof(buf), "%d – %d °C (AMS)",
                 (int)d->ams_nozzle_min_c, (int)d->ams_nozzle_max_c);
        _slot_row("Nozzle", buf);
    }

    // SpoolRecord-side — only when a record is mapped.
    if (d->has_spool) {
        if (d->weight_core_g > 0 || d->weight_advertised_g > 0 || d->weight_new_g > 0) {
            char aw[16] = "—", cw[16] = "—", nw[16] = "—";
            if (d->weight_advertised_g > 0) snprintf(aw, sizeof(aw), "%dg", (int)d->weight_advertised_g);
            if (d->weight_core_g       > 0) snprintf(cw, sizeof(cw), "%dg", (int)d->weight_core_g);
            if (d->weight_new_g        > 0) snprintf(nw, sizeof(nw), "%dg", (int)d->weight_new_g);
            snprintf(buf, sizeof(buf), "adv %s %s core %s %s new %s",
                     aw, LV_SYMBOL_BULLET, cw, LV_SYMBOL_BULLET, nw);
            _slot_row("Weights", buf);
        }
        if (d->density > 0.f) {
            snprintf(buf, sizeof(buf), "%.2f g/cm\xc2\xb3", (double)d->density);
            _slot_row("Density", buf);
        }
        if (d->color_name[0]) _slot_row("Colour",  d->color_name);
        if (d->slicer_filament[0]) _slot_row("Filament ID", d->slicer_filament);
        else if (d->ams_tray_info_idx[0]) _slot_row("Filament ID", d->ams_tray_info_idx);
        if (d->spool_tag_id[0]) _slot_row("Tag", d->spool_tag_id);
        else if (d->tag_uid[0]) _slot_row("Tag UID", d->tag_uid);
        if (d->spool_id[0])     _slot_row("Spool ID", d->spool_id);
    } else {
        // No record: surface the raw AMS metadata so the user knows what
        // the printer itself thinks is in the slot.
        if (d->ams_tray_info_idx[0]) _slot_row("Filament ID", d->ams_tray_info_idx);
        if (d->tag_uid[0])           _slot_row("Tag UID",     d->tag_uid);
    }

    // Note line across the bottom.
    lv_label_set_text(s_slot_note,
        (d->has_spool && d->note[0]) ? d->note : "");

    lv_screen_load(s_slot);
    lv_unlock();
}
