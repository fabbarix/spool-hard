#include "ui_wizard.h"
#include "ui.h"
#include "../../include/config.h"
#include "../../include/store.h"
#include "../../include/user_filaments_store.h"
#include "../../include/stock_filaments_store.h"
#include "../../include/scale_link.h"
#include "../../include/core_weights.h"
#include "../../include/quick_weights.h"
#include "fonts/spoolhard_fonts.h"
#include <Arduino.h>
#include <lvgl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "spoolhard/serial_mirror.h"

// Globals the wizard needs to talk to — defined elsewhere, declared here.
extern SpoolStore          g_store;
extern UserFilamentsStore  g_user_filaments;
extern StockFilamentsStore g_stock_filaments;
extern ScaleLink           g_scale;

// ── Theme tokens (match ui.cpp) ────────────────────────────
#define COL_BODY        lv_color_hex(0x05070b)
#define COL_CARD        lv_color_hex(0x1f2638)
#define COL_INPUT       lv_color_hex(0x0d1018)
#define COL_BORDER      lv_color_hex(0x3a4560)
#define COL_BRAND       lv_color_hex(0xf0b429)
#define COL_TEXT        lv_color_hex(0xe2e8f0)
#define COL_TEXT_MUTED  lv_color_hex(0xa9b8cc)   // >= 5.75:1 on every surface (was 0x64748b, 3.5:1)
#define COL_CONNECTED   lv_color_hex(0x2dd4bf)

// ── Wizard state machine ───────────────────────────────────

// Which "state" the user chose on the Full/Used/Empty step. Drives which
// weight screen renders and how weights are derived on Save.
enum SpoolState { SS_UNSET = 0, SS_FULL = 1, SS_USED = 2, SS_EMPTY = 3 };

// The whole wizard context is a single static struct owned by the UI task.
// Reset on ui_wizard_start; consumed on Save / Cancel. We intentionally do
// NOT queue a second tag scan while this is non-empty — dropped scans are
// logged by the caller.
struct WizardCtx {
    bool     active = false;
    // Tag fields copied from the SpoolTag argument.
    String   tag_uid;
    String   tag_format;
    String   tag_type;
    String   tag_url;
    // Seed values from the scanned tag (may be empty).
    String   hint_material;
    String   hint_brand;
    String   hint_color_hex;

    // User choices during the flow.
    bool       use_template    = false;
    String     template_id;
    int        template_core   = -1;  // rec.weight_core of the chosen template
    SpoolState spool_state     = SS_UNSET;
    int        advertised_g    = 0;   // user-chosen filament weight (Full)
    int        measured_g      = 0;   // latest captured scale reading
    int        core_g          = -1;  // derived or picked
    int        current_g       = -1;  // derived

    // Final record assembled before save (may be edited by wizard steps).
    String     final_material;
    String     final_subtype;
    String     final_brand;
    String     final_color_name;
    String     final_color_hex;
    String     final_setting_id;          // Filament preset ref (PFUL/PFUS), "" = none
    bool       via_color        = false;  // color step shown after filament pick (Back target)
    int        final_advertised = -1;
    int        final_core       = -1;
    int        final_current    = -1;
    int        final_new        = -1;
} s_wiz;

// ── Screen + widget handles ────────────────────────────────
static lv_obj_t* s_scr_prompt   = nullptr;
static lv_obj_t* s_scr_template = nullptr;
static lv_obj_t* s_scr_pick     = nullptr;
static lv_obj_t* s_scr_state    = nullptr;
static lv_obj_t* s_scr_full     = nullptr;
static lv_obj_t* s_scr_used     = nullptr;
static lv_obj_t* s_scr_empty    = nullptr;
static lv_obj_t* s_scr_usedlist = nullptr;
static lv_obj_t* s_scr_done     = nullptr;

// Live-weight readouts on the three weight screens.
static lv_obj_t* s_lbl_full_weight   = nullptr;
static lv_obj_t* s_lbl_used_weight   = nullptr;
static lv_obj_t* s_lbl_empty_weight  = nullptr;

// Dynamic children we need to rebuild per-entry.
static lv_obj_t* s_prompt_preview    = nullptr;
static lv_obj_t* s_pick_list         = nullptr;
static lv_obj_t* s_full_quicks       = nullptr;
static lv_obj_t* s_usedlist_list     = nullptr;
static lv_obj_t* s_done_summary      = nullptr;
static lv_obj_t* s_done_color        = nullptr;   // colour chip button

// Retained button pointers we need to toggle enabled/disabled (capture
// buttons are only live when we have a stable weight).
static lv_obj_t* s_btn_full_capture  = nullptr;
static lv_obj_t* s_btn_empty_capture = nullptr;
static lv_obj_t* s_btn_used_measure  = nullptr;
static lv_obj_t* s_btn_used_template = nullptr;   // "Use template core" shortcut

// Latest stable-weight snapshot driven by ui_wizard_on_weight().
static float   s_last_grams  = 0.f;
static String  s_last_state  = "";

// ── Small styling helpers ──────────────────────────────────

static void style_screen(lv_obj_t* s) {
    lv_obj_set_style_bg_color(s, COL_BODY, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, COL_TEXT, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt ? txt : "");
    lv_obj_set_style_text_color(lbl, color, 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

// Rounded button with an interior label. Event data carries an arbitrary
// integer opcode the handler switches on — mirrors the pattern in ui.cpp.
static lv_obj_t* make_btn(lv_obj_t* parent, const char* text, bool primary,
                          int op, lv_event_cb_t cb, int x, int y, int w, int h = 44) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, primary ? COL_BRAND : COL_INPUT, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t)op);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, primary ? COL_BODY : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &spoolhard_mont_16, 0);
    lv_obj_center(l);
    return b;
}

static void set_btn_enabled(lv_obj_t* b, bool enabled) {
    if (!b) return;
    if (enabled) lv_obj_clear_state(b, LV_STATE_DISABLED);
    else         lv_obj_add_state(b, LV_STATE_DISABLED);
    // Dim the whole button with an opacity that's still high enough to keep
    // the label readable against the amber/body backgrounds (LV_OPA_40 made
    // the text effectively invisible on top of a dark background).
    lv_obj_set_style_opa(b, enabled ? LV_OPA_COVER : LV_OPA_70, 0);
}

// Optional save-completion hook. Set by main.cpp via
// ui_wizard_set_save_callback so the LCD can move from "wizard owns
// the screen" to "spool detail screen + AMS-load latch armed" without
// the wizard module having to import the entire spool-detail rendering
// pipeline. nullptr = no hook → fall back to the legacy "back to home"
// behaviour.
static ui_wizard_save_cb_t s_save_cb = nullptr;

void ui_wizard_set_save_callback(ui_wizard_save_cb_t cb) { s_save_cb = cb; }

// Wizard navigation helpers — put the wizard out of the way when done.
// `saved_id` is the new spool's id when called from the Save path
// (drives the post-save callback); empty for cancel paths so they fall
// through to the legacy "back to home" behaviour.
static void close_wizard(const String& saved_id = String()) {
    String pending = saved_id;   // copy before reset wipes s_wiz
    s_wiz = WizardCtx{};
    if (pending.length() && s_save_cb) {
        s_save_cb(pending);      // main.cpp arms PendingAms + opens detail
    } else {
        ui_show_home();
    }
}

// ── Step navigation forward decls ──────────────────────────
static void show_prompt();
static void show_template();
static void show_pick();
static void show_filament_type();
static void show_filament_pick();
static void show_color(bool from_done);
static void show_state();
static void show_full();
static void show_used();
static void show_empty();
static void show_usedlist();
static void show_done();

// Op codes shared across screens — a tiny int lets us use one event
// callback per screen without needing separate functions per button.
//
// OP_NEW_FILAMENT is the new "Create new spool from a filament preset"
// branch (was OP_SCRATCH; the value stays 10 so on_template's switch
// doesn't shift). OP_DUPLICATE renames OP_COPY for the same reason.
// FILA_ROW is the click-event op for a row in the filament picker.
enum Op {
    OP_CREATE=1, OP_CLOSE=2,
    OP_NEW_FILAMENT=10, OP_DUPLICATE=11,
    OP_PICK_ROW=20, OP_BACK=21,
    OP_FILA_ROW=22,
    OP_CHIP_ALL=30, OP_CHIP_PLA=31, OP_CHIP_PETG=32, OP_CHIP_ABS=33, OP_CHIP_TPU=34,
    OP_CHIP_OTHER=35, OP_BRAND_ROW=36, OP_FILA_NEXT=37,
    OP_STATE_FULL=40, OP_STATE_USED=41, OP_STATE_EMPTY=42,
    OP_FULL_QUICK0=50, OP_FULL_QUICK1=51, OP_FULL_QUICK2=52,
    OP_FULL_QUICK3=53, OP_FULL_QUICK4=54, OP_FULL_QUICK5=55,
    OP_FULL_CAPTURE=56,
    OP_USED_PICKDB=60, OP_USED_MEASURE=61,
    OP_USED_ROW=62, OP_USED_TEMPLATE=63,
    OP_EMPTY_CAPTURE=70,
    OP_DONE_SAVE=80, OP_DONE_CANCEL=81, OP_DONE_COLOR=82,
    OP_COLOR_SW0=90,              // 90..(90+kColorCount-1): swatches
    OP_COLOR_SKIP=120,
};

// Utility to get the op from an lv_event user_data pointer.
static int event_op(lv_event_t* e) { return (int)(intptr_t)lv_event_get_user_data(e); }

// ── Step 1: new-tag prompt ─────────────────────────────────

static void on_prompt(lv_event_t* e) {
    switch (event_op(e)) {
        case OP_CREATE:
            // If the store already has spools, offer the template flow; if
            // not, skip straight to the state picker (or to Done when the
            // scale is disconnected).
            if (!g_store.list(0, 1).empty()) show_template();
            else                             show_state();
            return;
        case OP_CLOSE:
            Serial.println("[Wiz] cancelled at prompt — no record created");
            close_wizard();
            return;
    }
}

static void build_prompt() {
    s_scr_prompt = lv_obj_create(nullptr);
    style_screen(s_scr_prompt);

    lv_obj_t* title = make_label(s_scr_prompt, "New tag detected", COL_BRAND, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    s_prompt_preview = make_label(s_scr_prompt, "", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(s_prompt_preview, 16, 60);
    lv_obj_set_width(s_prompt_preview, 448);
    lv_label_set_long_mode(s_prompt_preview, LV_LABEL_LONG_WRAP);

    make_btn(s_scr_prompt, "Create entry", true,  OP_CREATE, on_prompt, 16,  236, 240);
    make_btn(s_scr_prompt, "Close",        false, OP_CLOSE,  on_prompt, 268, 236, 196);
}

static void show_prompt() {
    // Preview line: "UID · format · [hint_brand hint_material color]"
    String preview = "UID " + s_wiz.tag_uid;
    if (s_wiz.tag_format.length()) preview += "  " LV_SYMBOL_BULLET "  " + s_wiz.tag_format;
    if (s_wiz.hint_brand.length() || s_wiz.hint_material.length()) {
        preview += "\n";
        if (s_wiz.hint_brand.length())    preview += s_wiz.hint_brand + " ";
        if (s_wiz.hint_material.length()) preview += s_wiz.hint_material;
    }
    lv_lock();
    lv_label_set_text(s_prompt_preview, preview.c_str());
    lv_screen_load(s_scr_prompt);
    lv_unlock();
}

// ── Step 2: Duplicate spool vs New Filament ────────────────
//
// Two-button binary: pick a previous spool to copy from, OR pick a
// filament preset (custom for now; stock comes later when the build
// pipeline emits the JSONL sidecar). Either path seeds the
// material/brand/color fields and proceeds to the same weight-capture
// + save flow that already existed.

static void on_template(lv_event_t* e) {
    switch (event_op(e)) {
        case OP_NEW_FILAMENT:
            s_wiz.use_template     = false;
            s_wiz.template_id      = "";
            s_wiz.final_setting_id = "";
            // Seed material/brand from the tag's NDEF hints in case the
            // user picked a filament that doesn't carry them. Picker
            // will overwrite these on selection.
            s_wiz.final_material  = s_wiz.hint_material;
            s_wiz.final_brand     = s_wiz.hint_brand;
            // Default new-spool color: light grey per spec; the user
            // tweaks the real colour later in the web UI.
            s_wiz.final_color_hex = s_wiz.hint_color_hex.length()
                                    ? s_wiz.hint_color_hex
                                    : String("cccccc");
            show_filament_type();
            return;
        case OP_DUPLICATE:
            s_wiz.use_template = true;
            show_pick();
            return;
        case OP_BACK:
            show_prompt();
            return;
    }
}

static void build_template() {
    s_scr_template = lv_obj_create(nullptr);
    style_screen(s_scr_template);

    lv_obj_t* title = make_label(s_scr_template, "Create new spool", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    lv_obj_t* hint = make_label(s_scr_template,
        "Duplicate an existing spool's settings, or pick a filament\n"
        "preset to start fresh.", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(hint, 16, 56);

    // Order matches the user's flow description: Duplicate first,
    // New Filament second.
    make_btn(s_scr_template, "Duplicate",    false, OP_DUPLICATE,    on_template, 16,  140, 220, 56);
    make_btn(s_scr_template, "New filament", true,  OP_NEW_FILAMENT, on_template, 248, 140, 220, 56);
    make_btn(s_scr_template, "Back",         false, OP_BACK,         on_template, 16,  236, 100);
}

static void show_template() {
    lv_lock(); lv_screen_load(s_scr_template); lv_unlock();
}

// ── Step 3: template picker ────────────────────────────────

static String s_pick_material_filter = "";   // "" = all, else material code

static void on_pick(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) { show_template(); return; }
    if (op == OP_CHIP_ALL)  { s_pick_material_filter = "";     show_pick(); return; }
    if (op == OP_CHIP_PLA)  { s_pick_material_filter = "PLA";  show_pick(); return; }
    if (op == OP_CHIP_PETG) { s_pick_material_filter = "PETG"; show_pick(); return; }
    if (op == OP_CHIP_ABS)  { s_pick_material_filter = "ABS";  show_pick(); return; }
    if (op == OP_CHIP_TPU)  { s_pick_material_filter = "TPU";  show_pick(); return; }
    if (op == OP_PICK_ROW) {
        // The target button carries the spool id on its `user_data` via
        // a custom field; LVGL's event user_data is already the OP code,
        // so we stash the id on the object itself.
        lv_obj_t* t = lv_event_get_target_obj(e);
        const char* id = (const char*)lv_obj_get_user_data(t);
        if (!id) return;
        s_wiz.template_id = id;
        // Load the template record and seed the final fields.
        SpoolRecord rec;
        if (g_store.findById(String(id), rec)) {
            s_wiz.final_material    = rec.material_type;
            s_wiz.final_subtype     = rec.material_subtype;
            s_wiz.final_brand       = rec.brand;
            s_wiz.final_color_name  = rec.color_name;
            s_wiz.final_color_hex   = rec.color_code;
            s_wiz.final_advertised  = rec.weight_advertised;
            // Carry the template's measured empty-core over so the Used /
            // Empty screens can offer a one-tap "same as template" shortcut.
            s_wiz.template_core     = rec.weight_core;
        }
        show_state();
        return;
    }
}

static void build_pick() {
    s_scr_pick = lv_obj_create(nullptr);
    style_screen(s_scr_pick);

    lv_obj_t* title = make_label(s_scr_pick, "Pick a template", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 10);

    // Chip row — hard-coded short set; full search stays on the web.
    const char* chip_labels[] = { "All", "PLA", "PETG", "ABS", "TPU" };
    int chip_ops[]            = { OP_CHIP_ALL, OP_CHIP_PLA, OP_CHIP_PETG, OP_CHIP_ABS, OP_CHIP_TPU };
    int cx = 16;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t* chip = make_btn(s_scr_pick, chip_labels[i], false,
                                  chip_ops[i], on_pick, cx, 42, 72, 44);
        (void)chip;
        cx += 80;
    }

    s_pick_list = lv_list_create(s_scr_pick);
    lv_obj_set_pos(s_pick_list, 16, 92);
    lv_obj_set_size(s_pick_list, 448, 136);
    lv_obj_set_style_bg_color(s_pick_list, COL_CARD, 0);
    lv_obj_set_style_border_color(s_pick_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_pick_list, 1, 0);
    lv_obj_set_style_radius(s_pick_list, 10, 0);

    make_btn(s_scr_pick, "Back", false, OP_BACK, on_pick, 16, 236, 100);
}

// Owned string storage for list row ids. Cleared on every show_pick().
static std::vector<String*> s_pick_row_ids;

static void clear_pick_rows() {
    for (auto* s : s_pick_row_ids) delete s;
    s_pick_row_ids.clear();
    if (s_pick_list) lv_obj_clean(s_pick_list);
}

static void show_pick() {
    lv_lock();
    clear_pick_rows();
    auto rows = g_store.list(0, 200, s_pick_material_filter);
    if (rows.empty()) {
        lv_obj_t* empty = make_label(s_pick_list, "(no spools match)", COL_TEXT_MUTED,
                                     &spoolhard_mont_16);
        lv_obj_center(empty);
    } else {
        for (const auto& rec : rows) {
            String line = rec.brand.length() ? rec.brand : String("Unknown");
            if (rec.material_type.length()) line += "  " LV_SYMBOL_BULLET "  " + rec.material_type;
            if (rec.color_name.length())    line += "  " LV_SYMBOL_BULLET "  " + rec.color_name;
            lv_obj_t* btn = lv_list_add_btn(s_pick_list, LV_SYMBOL_RIGHT, line.c_str());
            // Stash the spool id as a heap String so the callback can use it.
            String* idCopy = new String(rec.id);
            s_pick_row_ids.push_back(idCopy);
            lv_obj_set_user_data(btn, (void*)idCopy->c_str());
            lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED,
                                (void*)(intptr_t)OP_PICK_ROW);
        }
    }
    lv_screen_load(s_scr_pick);
    lv_unlock();
}

// ── Step 3a: filament picker (New Filament branch) ─────────
//
// Three screens: Type -> Brand -> list. Rows come from the user filament
// store (badged with a pencil) then the stock library. Type "Other" is
// every filament_type outside the four named ones. The Brand step is
// skipped when the chosen type has at most one brand.
//
// Scroll-tap safety: a row tap only highlights; the fixed Next button
// commits. Taps landing while the list scrolls or within
// kTapAfterScrollMs of a scroll ending (the tap that stops a fling) are
// ignored. On selection the wizard seeds material/brand/subtype +
// setting_id and moves on to the colour step.

static constexpr uint32_t kTapAfterScrollMs = 300;
static const char* kTypeNames[] = { "PLA", "PETG", "ABS", "TPU" };

static lv_obj_t*               s_scr_ftype        = nullptr;
static lv_obj_t*               s_scr_brand        = nullptr;
static lv_obj_t*               s_brand_grid       = nullptr;
static lv_obj_t*               s_brand_title      = nullptr;
static lv_obj_t*               s_scr_filapick     = nullptr;
static lv_obj_t*               s_filapick_list    = nullptr;
static lv_obj_t*               s_filapick_title   = nullptr;
static lv_obj_t*               s_filapick_next    = nullptr;
static String                  s_filapick_filter  = "";   // "", PLA.., "*other"
static String                  s_filapick_brand   = "";   // "" = all brands
static bool                    s_brand_skipped    = false;
static String                  s_filapick_sel;            // highlighted setting_id
static lv_obj_t*               s_filapick_sel_btn = nullptr;
static bool                    s_filapick_scrolling = false;
static uint32_t                s_filapick_scroll_end_ms = 0;
// Owned heap-Strings for setting_ids / brand names attached as user_data
// on dynamic buttons. Freed on every rebuild of their container.
static std::vector<String*>    s_filapick_row_ids;
static std::vector<String*>    s_brand_names;

static void show_filament_type();
static void show_brand();

static bool type_matches(const FilamentRecord& r) {
    if (s_filapick_filter.isEmpty()) return true;
    if (s_filapick_filter != "*other") return r.filament_type == s_filapick_filter;
    for (const char* t : kTypeNames) if (r.filament_type == t) return false;
    return true;
}

// User rows first (more relevant), then stock; type-filtered only.
static void collect_filaments(std::vector<FilamentRecord>& user,
                              std::vector<FilamentRecord>& stock) {
    String f = (s_filapick_filter == "*other") ? String("") : s_filapick_filter;
    user  = g_user_filaments.list(0, 200, f);
    stock = g_stock_filaments.list(0, 200, f);
    auto drop = [](std::vector<FilamentRecord>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                [](const FilamentRecord& r) { return !type_matches(r); }), v.end());
    };
    drop(user); drop(stock);
}

// ── Type ──

static void on_ftype(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) { show_template(); return; }
    if (op == OP_CHIP_ALL)        s_filapick_filter = "";
    else if (op == OP_CHIP_OTHER) s_filapick_filter = "*other";
    else if (op >= OP_CHIP_PLA && op <= OP_CHIP_TPU) s_filapick_filter = kTypeNames[op - OP_CHIP_PLA];
    else return;
    show_brand();
}

static void build_filament_type() {
    s_scr_ftype = lv_obj_create(nullptr);
    style_screen(s_scr_ftype);
    lv_obj_t* title = make_label(s_scr_ftype, "Filament type", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 10);
    const char* labels[] = { "PLA", "PETG", "ABS", "TPU", "Other", "All" };
    int ops[] = { OP_CHIP_PLA, OP_CHIP_PETG, OP_CHIP_ABS, OP_CHIP_TPU, OP_CHIP_OTHER, OP_CHIP_ALL };
    // 3 x 2 grid of 144x80 buttons (>= 64 px targets).
    for (int i = 0; i < 6; ++i)
        make_btn(s_scr_ftype, labels[i], false, ops[i], on_ftype,
                 16 + (i % 3) * 152, 50 + (i / 3) * 90, 144, 80);
    make_btn(s_scr_ftype, "Back", false, OP_BACK, on_ftype, 16, 236, 100);
}

static void show_filament_type() {
    lv_lock();
    if (!s_scr_ftype) build_filament_type();
    lv_screen_load(s_scr_ftype);
    lv_unlock();
}

// ── Brand ──

static void on_brand(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) { show_filament_type(); return; }
    if (op == OP_BRAND_ROW) {
        const char* b = (const char*)lv_obj_get_user_data(lv_event_get_target_obj(e));
        s_filapick_brand = b ? b : "";
        s_filapick_sel = "";
        show_filament_pick();
    }
}

static void build_brand() {
    s_scr_brand = lv_obj_create(nullptr);
    style_screen(s_scr_brand);
    s_brand_title = make_label(s_scr_brand, "", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(s_brand_title, 16, 10);
    // Scrollable 2-column grid; only scrolls past 6 brands.
    s_brand_grid = lv_obj_create(s_scr_brand);
    lv_obj_set_pos(s_brand_grid, 16, 46);
    lv_obj_set_size(s_brand_grid, 448, 184);   // 3 rows x 56 + 2 x 8 gaps
    lv_obj_set_style_bg_opa(s_brand_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_brand_grid, 0, 0);
    lv_obj_set_style_pad_all(s_brand_grid, 0, 0);
    lv_obj_set_style_pad_row(s_brand_grid, 8, 0);
    lv_obj_set_style_pad_column(s_brand_grid, 8, 0);
    lv_obj_set_flex_flow(s_brand_grid, LV_FLEX_FLOW_ROW_WRAP);
    make_btn(s_scr_brand, "Back", false, OP_BACK, on_brand, 16, 236, 100);
}

static void show_brand() {
    std::vector<FilamentRecord> user, stock;
    collect_filaments(user, stock);
    // Brand -> count, most filaments first (stable: ties keep first-seen).
    std::vector<std::pair<String, int>> brands;
    auto add = [&](const FilamentRecord& r) {
        for (auto& b : brands) if (b.first == r.filament_vendor) { ++b.second; return; }
        brands.push_back({ r.filament_vendor, 1 });
    };
    for (const auto& r : user)  add(r);
    for (const auto& r : stock) add(r);
    std::stable_sort(brands.begin(), brands.end(),
                     [](const std::pair<String, int>& a, const std::pair<String, int>& b) {
                         return a.second > b.second;
                     });

    s_filapick_sel = "";
    if (brands.size() <= 1) {
        s_brand_skipped  = true;
        s_filapick_brand = "";
        show_filament_pick();
        return;
    }
    s_brand_skipped = false;

    lv_lock();
    if (!s_scr_brand) build_brand();
    for (auto* s : s_brand_names) delete s;
    s_brand_names.clear();
    lv_obj_clean(s_brand_grid);
    String t = "Brand";
    if (s_filapick_filter.length())
        t += "  " LV_SYMBOL_BULLET "  " + (s_filapick_filter == "*other" ? String("Other") : s_filapick_filter);
    lv_label_set_text(s_brand_title, t.c_str());
    auto add_btn = [&](const String& name, const String& label) {
        lv_obj_t* b = make_btn(s_brand_grid, label.c_str(), false, OP_BRAND_ROW, on_brand, 0, 0, 220, 56);
        String* n = new String(name);
        s_brand_names.push_back(n);
        lv_obj_set_user_data(b, (void*)n->c_str());
    };
    add_btn("", "All brands");
    for (const auto& b : brands)
        add_btn(b.first, (b.first.length() ? b.first : String("(no brand)")) + " (" + b.second + ")");
    lv_obj_scroll_to_y(s_brand_grid, 0, LV_ANIM_OFF);
    lv_screen_load(s_scr_brand);
    lv_unlock();
}

// ── List ──

static void clear_filapick_rows() {
    for (auto* s : s_filapick_row_ids) delete s;
    s_filapick_row_ids.clear();
    s_filapick_sel_btn = nullptr;
    if (s_filapick_list) lv_obj_clean(s_filapick_list);
}

static void filapick_highlight(lv_obj_t* btn) {
    if (s_filapick_sel_btn) lv_obj_remove_state(s_filapick_sel_btn, LV_STATE_CHECKED);
    s_filapick_sel_btn = btn;
    if (btn) lv_obj_add_state(btn, LV_STATE_CHECKED);
    set_btn_enabled(s_filapick_next, btn != nullptr);
}

static void on_filapick_scroll(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        s_filapick_scrolling = true;
    } else {
        s_filapick_scrolling = false;
        s_filapick_scroll_end_ms = millis();
    }
}

static void on_filament_pick(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) {
        if (s_brand_skipped) show_filament_type(); else show_brand();
        return;
    }
    if (op == OP_FILA_ROW) {
        if (s_filapick_scrolling ||
            millis() - s_filapick_scroll_end_ms < kTapAfterScrollMs) return;
        lv_obj_t* t = lv_event_get_target_obj(e);
        const char* sid = (const char*)lv_obj_get_user_data(t);
        if (!sid) return;
        s_filapick_sel = sid;
        filapick_highlight(t);
        return;
    }
    if (op == OP_FILA_NEXT) {
        if (s_filapick_sel.isEmpty()) return;
        FilamentRecord rec;
        // User store first — locally-edited presets win over their
        // stock parents if the same id ever appears in both.
        bool found = g_user_filaments.findById(s_filapick_sel, rec) ||
                     g_stock_filaments.findById(s_filapick_sel, rec);
        if (found) {
            s_wiz.final_setting_id  = rec.setting_id;
            if (rec.filament_type.length())   s_wiz.final_material = rec.filament_type;
            if (rec.filament_subtype.length())s_wiz.final_subtype  = rec.filament_subtype;
            if (rec.filament_vendor.length()) s_wiz.final_brand    = rec.filament_vendor;
            // Filaments don't carry a single canonical colour, so the
            // colour step comes next (Skip keeps the tag hint / grey).
        }
        show_color(false);
        return;
    }
}

static void build_filament_pick() {
    s_scr_filapick = lv_obj_create(nullptr);
    style_screen(s_scr_filapick);

    s_filapick_title = make_label(s_scr_filapick, "", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(s_filapick_title, 16, 10);
    lv_obj_set_width(s_filapick_title, 448);
    lv_label_set_long_mode(s_filapick_title, LV_LABEL_LONG_DOT);

    s_filapick_list = lv_list_create(s_scr_filapick);
    lv_obj_set_pos(s_filapick_list, 16, 46);
    lv_obj_set_size(s_filapick_list, 448, 182);
    lv_obj_set_style_bg_color(s_filapick_list, COL_CARD, 0);
    lv_obj_set_style_border_color(s_filapick_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_filapick_list, 1, 0);
    lv_obj_set_style_radius(s_filapick_list, 10, 0);
    lv_obj_add_event_cb(s_filapick_list, on_filapick_scroll, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(s_filapick_list, on_filapick_scroll, LV_EVENT_SCROLL_END, nullptr);

    make_btn(s_scr_filapick, "Back", false, OP_BACK, on_filament_pick, 16, 236, 100);
    s_filapick_next = make_btn(s_scr_filapick, "Next", true, OP_FILA_NEXT, on_filament_pick, 324, 236, 140);
}

static void show_filament_pick() {
    std::vector<FilamentRecord> user_rows, stock_rows;
    collect_filaments(user_rows, stock_rows);
    lv_lock();
    if (!s_scr_filapick) build_filament_pick();
    clear_filapick_rows();
    s_filapick_scrolling = false;

    String t = s_filapick_brand.length() ? s_filapick_brand : String("All brands");
    if (s_filapick_filter.length())
        t += "  " LV_SYMBOL_BULLET "  " + (s_filapick_filter == "*other" ? String("Other") : s_filapick_filter);
    lv_label_set_text(s_filapick_title, t.c_str());

    lv_obj_t* sel_btn = nullptr;
    auto append_row = [&](const FilamentRecord& rec, const char* badge) {
        if (s_filapick_brand.length() && rec.filament_vendor != s_filapick_brand) return;
        String line = rec.name.length() ? rec.name : String("(unnamed)");
        if (badge && *badge) line = String(badge) + "  " + line;
        if (s_filapick_filter.isEmpty() || s_filapick_filter == "*other")
            if (rec.filament_type.length()) line += "  " LV_SYMBOL_BULLET "  " + rec.filament_type;
        if (s_filapick_brand.isEmpty() && rec.filament_vendor.length())
            line += "  " LV_SYMBOL_BULLET "  " + rec.filament_vendor;
        lv_obj_t* btn = lv_list_add_btn(s_filapick_list, nullptr, line.c_str());
        lv_obj_set_style_min_height(btn, 48, 0);
        // Highlighted row: amber fill, dark text.
        lv_obj_set_style_bg_color(btn, COL_BRAND, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, COL_BODY, LV_STATE_CHECKED);
        String* idCopy = new String(rec.setting_id);
        s_filapick_row_ids.push_back(idCopy);
        lv_obj_set_user_data(btn, (void*)idCopy->c_str());
        lv_obj_add_event_cb(btn, on_filament_pick, LV_EVENT_CLICKED,
                            (void*)(intptr_t)OP_FILA_ROW);
        if (s_filapick_sel.length() && rec.setting_id == s_filapick_sel) sel_btn = btn;
    };

    if (user_rows.empty() && stock_rows.empty()) {
        // True empty state — no stock library uploaded AND no user
        // filaments created (or none of this type).
        lv_obj_t* empty = make_label(s_filapick_list,
            "No filaments available.\n\n"
            "Upload the stock filament library or create a custom one\n"
            "in the web UI's Filaments tab, then re-scan.",
            COL_TEXT_MUTED, &spoolhard_mont_16);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
    } else {
        for (const auto& rec : user_rows)  append_row(rec, LV_SYMBOL_EDIT);    // pencil
        for (const auto& rec : stock_rows) append_row(rec, "");
    }
    // Coming back (from colour) keeps the previous pick highlighted.
    if (!sel_btn) s_filapick_sel = "";
    filapick_highlight(sel_btn);
    if (sel_btn) lv_obj_scroll_to_view(sel_btn, LV_ANIM_OFF);
    else         lv_obj_scroll_to_y(s_filapick_list, 0, LV_ANIM_OFF);
    lv_screen_load(s_scr_filapick);
    lv_unlock();
}

// ── Step 3b: spool colour (swatch grid) ────────────────────
//
// Fixed palette of common filament colours, 3 x 5 swatches. One tap sets
// colour hex + name. Shown after the filament pick (New filament path) and
// from the Done screen's colour chip (both paths); Duplicate already copied
// the source spool's colour so it skips the step.

struct ColorSwatch { const char* name; const char* hex; };
static const ColorSwatch kColors[] = {
    { "White",      "FFFFFF" }, { "Black",  "000000" }, { "Gray",   "8E9089" },
    { "Silver",     "A6A9AA" }, { "Beige",  "F7E6DE" },
    { "Red",        "C12E1F" }, { "Orange", "FF6A13" }, { "Yellow", "F4EE2A" },
    { "Green",      "00AE42" }, { "Dark Green", "1F5E3A" },
    { "Blue",       "0A2989" }, { "Light Blue", "56B7E6" }, { "Purple", "5E43B7" },
    { "Pink",       "F55A74" }, { "Brown",  "7D5236" },
};
static constexpr int kColorCount = sizeof(kColors) / sizeof(kColors[0]);

static lv_obj_t* s_scr_color = nullptr;
static lv_obj_t* s_color_sw[kColorCount] = {};
static lv_obj_t* s_color_skip = nullptr;
static bool      s_color_from_done = false;

// Black or white text, whichever has the higher WCAG contrast on the
// swatch. Black wins when relative luminance > ~0.179 (a plain luma
// cutoff put white on orange/green/pink at ~3:1).
static float srgb_lin(uint32_t c8) {
    float c = c8 / 255.f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static lv_color_t text_on(uint32_t rgb) {
    float L = 0.2126f * srgb_lin((rgb >> 16) & 0xff) +
              0.7152f * srgb_lin((rgb >> 8) & 0xff) +
              0.0722f * srgb_lin(rgb & 0xff);
    return (L + 0.05f) / 0.05f >= 1.05f / (L + 0.05f) ? lv_color_hex(0x000000)
                                                      : lv_color_hex(0xffffff);
}

static void on_color(lv_event_t* e) {
    int op = event_op(e);
    if (op >= OP_COLOR_SW0 && op < OP_COLOR_SW0 + kColorCount) {
        const ColorSwatch& c = kColors[op - OP_COLOR_SW0];
        s_wiz.final_color_name = c.name;
        s_wiz.final_color_hex  = c.hex;
    } else if (op == OP_BACK && !s_color_from_done) {
        show_filament_pick();
        return;
    }
    // Swatch, Skip, or Back from the Done chip.
    if (s_color_from_done) show_done();
    else                   show_state();
}

static void build_color() {
    s_scr_color = lv_obj_create(nullptr);
    style_screen(s_scr_color);

    lv_obj_t* title = make_label(s_scr_color, "Spool color", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 10);

    // 5 x 84 px + 4 x 7 px gaps = 448; rows 52 px tall (>= 44 px targets).
    for (int i = 0; i < kColorCount; ++i) {
        int col = i % 5, row = i / 5;
        lv_obj_t* b = make_btn(s_scr_color, kColors[i].name, false, OP_COLOR_SW0 + i,
                               on_color, 16 + col * 91, 46 + row * 60, 84, 52);
        uint32_t rgb = strtoul(kColors[i].hex, nullptr, 16);
        lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
        // Constant 3 px border; only its colour marks the selection.
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_border_color(b, COL_BORDER, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t* l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_color(l, text_on(rgb), 0);
        lv_obj_set_width(l, 76);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        s_color_sw[i] = b;
    }

    make_btn(s_scr_color, "Back", false, OP_BACK, on_color, 16, 236, 100);
    s_color_skip = make_btn(s_scr_color, "Skip", false, OP_COLOR_SKIP, on_color, 364, 236, 100);
}

static void show_color(bool from_done) {
    s_color_from_done = from_done;
    if (!from_done) s_wiz.via_color = true;
    lv_lock();
    for (int i = 0; i < kColorCount; ++i) {
        bool sel = s_wiz.final_color_hex.equalsIgnoreCase(kColors[i].hex);
        lv_obj_set_style_border_color(s_color_sw[i], sel ? COL_BRAND : COL_BORDER, 0);
    }
    // From Done, Back already means "keep the current colour".
    if (from_done) lv_obj_add_flag(s_color_skip, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(s_color_skip, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(s_scr_color);
    lv_unlock();
}

// ── Step 4: spool state (Full / Used / Empty) ──────────────

static void on_state(lv_event_t* e) {
    switch (event_op(e)) {
        case OP_STATE_FULL:  s_wiz.spool_state = SS_FULL;  show_full();  return;
        case OP_STATE_USED:  s_wiz.spool_state = SS_USED;  show_used();  return;
        case OP_STATE_EMPTY: s_wiz.spool_state = SS_EMPTY; show_empty(); return;
        case OP_BACK:
            if (s_wiz.use_template)   show_pick();
            else if (s_wiz.via_color) show_color(false);
            else                      show_template();
            return;
    }
}

static void build_state() {
    s_scr_state = lv_obj_create(nullptr);
    style_screen(s_scr_state);

    lv_obj_t* title = make_label(s_scr_state, "Spool state?", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    lv_obj_t* hint = make_label(s_scr_state,
        "Place the spool on the scale; we'll learn the core weight "
        "from the measurement.", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(hint, 16, 50);
    lv_obj_set_width(hint, 448);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    make_btn(s_scr_state, "Full spool",  true,  OP_STATE_FULL,  on_state, 16,  120, 140, 72);
    make_btn(s_scr_state, "Used spool",  false, OP_STATE_USED,  on_state, 170, 120, 140, 72);
    make_btn(s_scr_state, "Empty spool", false, OP_STATE_EMPTY, on_state, 324, 120, 140, 72);

    make_btn(s_scr_state, "Back", false, OP_BACK, on_state, 16, 236, 100);
}

static void show_state() {
    // If the scale isn't connected we can't weigh anything — jump straight
    // to Done with the info we already have.
    if (!g_scale.isConnected()) {
        s_wiz.spool_state = SS_UNSET;
        show_done();
        return;
    }
    lv_lock(); lv_screen_load(s_scr_state); lv_unlock();
}

// ── Step 5a: Full spool — quick-weight selection + capture ─

static int s_full_quicks_cache[QUICK_WEIGHTS_MAX] = {0};

static void on_full(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) { show_state(); return; }
    if (op >= OP_FULL_QUICK0 && op <= OP_FULL_QUICK5) {
        int idx = op - OP_FULL_QUICK0;
        s_wiz.advertised_g   = s_full_quicks_cache[idx];
        s_wiz.final_advertised = s_wiz.advertised_g;
        // Capture goes live the moment a quick-weight is chosen; we don't
        // gate on "stable" any more — the user reads the screen and
        // decides when to commit.
        set_btn_enabled(s_btn_full_capture, s_wiz.advertised_g > 0);
        return;
    }
    if (op == OP_FULL_CAPTURE) {
        if (s_wiz.advertised_g <= 0 || s_last_grams <= 0) return;
        int measured = (int)s_last_grams;
        int core = measured - s_wiz.advertised_g;
        if (core < 0) core = 0;   // clamp — something's off if this happens
        s_wiz.measured_g  = measured;
        s_wiz.core_g      = core;
        s_wiz.current_g   = s_wiz.advertised_g;
        s_wiz.final_core  = core;
        s_wiz.final_current = s_wiz.advertised_g;
        s_wiz.final_new     = s_wiz.advertised_g;
        // Auto-learn empty-core weight so next time this (brand, material,
        // advertised) is registered we can pre-fill.
        if (s_wiz.final_brand.length() && s_wiz.final_material.length()) {
            CoreWeights::set(s_wiz.final_brand, s_wiz.final_material,
                             s_wiz.advertised_g, core);
        }
        show_done();
        return;
    }
}

static void build_full() {
    s_scr_full = lv_obj_create(nullptr);
    style_screen(s_scr_full);

    lv_obj_t* title = make_label(s_scr_full, "Full spool " LV_SYMBOL_BULLET " advertised weight", COL_TEXT, &spoolhard_mont_18);
    lv_obj_set_pos(title, 16, 14);

    // Container for the quick-weight chips; populated at show-time from NVS.
    s_full_quicks = lv_obj_create(s_scr_full);
    lv_obj_set_pos(s_full_quicks, 16, 52);
    lv_obj_set_size(s_full_quicks, 448, 80);
    lv_obj_set_style_bg_opa(s_full_quicks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_full_quicks, 0, 0);
    lv_obj_set_style_pad_all(s_full_quicks, 0, 0);
    lv_obj_clear_flag(s_full_quicks, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_full_weight = make_label(s_scr_full, "Scale: --", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(s_lbl_full_weight, 16, 150);

    s_btn_full_capture = make_btn(s_scr_full, "Capture", true, OP_FULL_CAPTURE, on_full,
                                  16, 236, 200);
    set_btn_enabled(s_btn_full_capture, false);
    make_btn(s_scr_full, "Back", false, OP_BACK, on_full, 240, 236, 100);
}

static void show_full() {
    lv_lock();
    // Rebuild quick-weight buttons from current NVS list.
    lv_obj_clean(s_full_quicks);
    auto grams = QuickWeights::get();
    for (size_t i = 0; i < grams.size() && i < QUICK_WEIGHTS_MAX; ++i) {
        s_full_quicks_cache[i] = grams[i];
        char label[12]; snprintf(label, sizeof(label), "%d g", grams[i]);
        int x = (int)i * 100;
        if (x + 92 > 448) break;  // ran out of row
        make_btn(s_full_quicks, label, false, OP_FULL_QUICK0 + (int)i, on_full,
                 x, 0, 92, 44);
    }
    s_wiz.advertised_g = 0;
    set_btn_enabled(s_btn_full_capture, false);
    lv_screen_load(s_scr_full);
    lv_unlock();
}

// ── Step 5b: Used spool ────────────────────────────────────

static void on_used(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK)        { show_state(); return; }
    if (op == OP_USED_PICKDB) { show_usedlist(); return; }
    if (op == OP_USED_TEMPLATE) {
        // Template's weight_core is authoritative — user just confirmed
        // "it's the same kind of spool". Still need a live reading to
        // derive the current filament mass (measured − core).
        if (s_wiz.template_core <= 0 || s_last_grams <= 0) return;
        s_wiz.core_g        = s_wiz.template_core;
        s_wiz.final_core    = s_wiz.template_core;
        int filament = (int)s_last_grams - s_wiz.template_core;
        if (filament < 0) filament = 0;
        s_wiz.final_current = filament;
        show_done();
        return;
    }
    if (op == OP_USED_MEASURE) {
        // Take the current scale reading as the empty-core reference. The
        // user is the judge of whether it's settled — the displayed state
        // next to the weight tells them.
        if (s_last_grams <= 0) return;
        s_wiz.core_g    = (int)s_last_grams;
        s_wiz.measured_g = 0;
        s_wiz.final_core    = s_wiz.core_g;
        s_wiz.final_current = -1;  // unknown; user can re-weigh later
        if (s_wiz.final_brand.length() && s_wiz.final_material.length() &&
            s_wiz.final_advertised > 0) {
            CoreWeights::set(s_wiz.final_brand, s_wiz.final_material,
                             s_wiz.final_advertised, s_wiz.core_g);
        }
        show_done();
        return;
    }
}

static void build_used() {
    s_scr_used = lv_obj_create(nullptr);
    style_screen(s_scr_used);

    lv_obj_t* title = make_label(s_scr_used, "Used spool", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    lv_obj_t* hint = make_label(s_scr_used,
        "Pick a known empty-core weight or take the current scale reading "
        "as the empty-core reference. If a template was chosen, its core "
        "is offered as a one-tap shortcut.",
        COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(hint, 16, 46);
    lv_obj_set_width(hint, 448);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    // Template shortcut — full width, primary. Re-labelled + shown at
    // show_used time based on whether s_wiz.template_core is set.
    s_btn_used_template = make_btn(s_scr_used, "Use template core", true,
                                   OP_USED_TEMPLATE, on_used, 16, 96, 448, 44);

    make_btn(s_scr_used, "Pick from DB", true,  OP_USED_PICKDB,  on_used, 16,  148, 220, 44);
    s_btn_used_measure =
    make_btn(s_scr_used, "Measure core", false, OP_USED_MEASURE, on_used, 244, 148, 220, 44);

    s_lbl_used_weight = make_label(s_scr_used, "Scale: --", COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(s_lbl_used_weight, 16, 204);

    make_btn(s_scr_used, "Back", false, OP_BACK, on_used, 16, 236, 100);
}

static void show_used() {
    lv_lock();
    // Template shortcut is only meaningful when the user picked a template
    // that actually had a measured core on it. Relabel with the grams so
    // the user can verify at a glance before tapping.
    if (s_btn_used_template) {
        if (s_wiz.template_core > 0) {
            lv_obj_clear_flag(s_btn_used_template, LV_OBJ_FLAG_HIDDEN);
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "Use template core: %d g", s_wiz.template_core);
            // The label is the first child we created inside make_btn.
            lv_obj_t* l = lv_obj_get_child(s_btn_used_template, 0);
            if (l) lv_label_set_text(l, lbl);
            set_btn_enabled(s_btn_used_template, s_last_grams > 0);
        } else {
            lv_obj_add_flag(s_btn_used_template, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_screen_load(s_scr_used);
    lv_unlock();
    set_btn_enabled(s_btn_used_measure, s_last_grams > 0);
}

// Used-list picker — shows matching entries from the core-weights DB.
static std::vector<String*> s_used_keys;
static void clear_used_rows() {
    for (auto* s : s_used_keys) delete s;
    s_used_keys.clear();
    if (s_usedlist_list) lv_obj_clean(s_usedlist_list);
}

static void on_usedlist(lv_event_t* e) {
    int op = event_op(e);
    if (op == OP_BACK) { show_used(); return; }
    if (op == OP_USED_ROW) {
        lv_obj_t* t = lv_event_get_target_obj(e);
        const char* key = (const char*)lv_obj_get_user_data(t);
        if (!key) return;
        // Find the matching DB entry and pull out grams.
        for (const auto& e : CoreWeights::list()) {
            String k = CoreWeights::keyFor(e.brand, e.material, e.advertised);
            if (k == key) {
                s_wiz.core_g           = e.grams;
                s_wiz.final_core       = e.grams;
                s_wiz.final_brand      = e.brand;
                s_wiz.final_material   = e.material;
                s_wiz.final_advertised = e.advertised;
                s_wiz.final_current    = -1;
                show_done();
                return;
            }
        }
    }
}

static void build_usedlist() {
    s_scr_usedlist = lv_obj_create(nullptr);
    style_screen(s_scr_usedlist);

    lv_obj_t* title = make_label(s_scr_usedlist, "Core weight DB", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    s_usedlist_list = lv_list_create(s_scr_usedlist);
    lv_obj_set_pos(s_usedlist_list, 16, 54);
    lv_obj_set_size(s_usedlist_list, 448, 168);
    lv_obj_set_style_bg_color(s_usedlist_list, COL_CARD, 0);
    lv_obj_set_style_border_color(s_usedlist_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_usedlist_list, 1, 0);
    lv_obj_set_style_radius(s_usedlist_list, 10, 0);

    make_btn(s_scr_usedlist, "Back", false, OP_BACK, on_usedlist, 16, 236, 100);
}

static void show_usedlist() {
    lv_lock();
    clear_used_rows();
    auto entries = CoreWeights::list();
    if (entries.empty()) {
        lv_obj_t* empty = make_label(s_usedlist_list,
            "(no core weights learned yet — use Measure core instead)",
            COL_TEXT_MUTED, &spoolhard_mont_16);
        lv_obj_center(empty);
    } else {
        for (const auto& e : entries) {
            char line[96];
            snprintf(line, sizeof(line),
                     "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET
                     "  %dg " LV_SYMBOL_RIGHT " core %dg",
                     e.brand.c_str(), e.material.c_str(), e.advertised, e.grams);
            lv_obj_t* btn = lv_list_add_btn(s_usedlist_list, LV_SYMBOL_RIGHT, line);
            String* key = new String(CoreWeights::keyFor(e.brand, e.material, e.advertised));
            s_used_keys.push_back(key);
            lv_obj_set_user_data(btn, (void*)key->c_str());
            lv_obj_add_event_cb(btn, on_usedlist, LV_EVENT_CLICKED,
                                (void*)(intptr_t)OP_USED_ROW);
        }
    }
    lv_screen_load(s_scr_usedlist);
    lv_unlock();
}

// ── Step 5c: Empty spool ───────────────────────────────────

static void on_empty(lv_event_t* e) {
    switch (event_op(e)) {
        case OP_BACK: show_state(); return;
        case OP_EMPTY_CAPTURE: {
            if (s_last_grams <= 0) return;   // no reading yet, nothing to capture
            int measured = (int)s_last_grams;
            s_wiz.measured_g = measured;
            s_wiz.core_g     = measured;
            s_wiz.current_g  = 0;
            s_wiz.final_core    = measured;
            s_wiz.final_current = 0;
            if (s_wiz.final_brand.length() && s_wiz.final_material.length() &&
                s_wiz.final_advertised > 0) {
                CoreWeights::set(s_wiz.final_brand, s_wiz.final_material,
                                 s_wiz.final_advertised, measured);
            }
            show_done();
            return;
        }
    }
}

static void build_empty() {
    s_scr_empty = lv_obj_create(nullptr);
    style_screen(s_scr_empty);

    lv_obj_t* title = make_label(s_scr_empty, "Empty spool", COL_TEXT, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    lv_obj_t* hint = make_label(s_scr_empty,
        "Place the empty spool on the scale and wait for a stable reading.",
        COL_TEXT_MUTED, &spoolhard_mont_16);
    lv_obj_set_pos(hint, 16, 54);
    lv_obj_set_width(hint, 448);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    s_lbl_empty_weight = make_label(s_scr_empty, "Scale: --", COL_BRAND, &spoolhard_mont_28);
    lv_obj_set_pos(s_lbl_empty_weight, 16, 110);

    s_btn_empty_capture = make_btn(s_scr_empty, "Capture", true, OP_EMPTY_CAPTURE, on_empty,
                                   16, 236, 200);
    make_btn(s_scr_empty, "Back", false, OP_BACK, on_empty, 240, 236, 100);
}

static void show_empty() {
    lv_lock(); lv_screen_load(s_scr_empty); lv_unlock();
    // Capture starts enabled; it just won't fire if there's no reading yet.
    // Visual state of the weight label tells the user whether it's settled.
    set_btn_enabled(s_btn_empty_capture, s_last_grams > 0);
}

// ── Step 6: summary / save ─────────────────────────────────

static void on_done(lv_event_t* e) {
    switch (event_op(e)) {
        case OP_DONE_CANCEL:
            Serial.println("[Wiz] cancelled on summary");
            close_wizard();
            return;
        case OP_DONE_COLOR:
            show_color(true);
            return;
        case OP_DONE_SAVE: {
            SpoolRecord rec;
            // id / tag_id always come from the newly-scanned tag.
            rec.id     = s_wiz.tag_uid;
            rec.tag_id = s_wiz.tag_uid;
            rec.material_type     = s_wiz.final_material;
            rec.material_subtype  = s_wiz.final_subtype;
            rec.brand             = s_wiz.final_brand;
            rec.color_name        = s_wiz.final_color_name;
            rec.color_code        = s_wiz.final_color_hex;
            rec.weight_advertised = s_wiz.final_advertised;
            rec.weight_core       = s_wiz.final_core;
            rec.weight_current    = s_wiz.final_current;
            rec.weight_new        = s_wiz.final_new;
            rec.setting_id        = s_wiz.final_setting_id;
            rec.data_origin       = s_wiz.tag_format;
            rec.tag_type          = s_wiz.tag_type;
            g_store.upsert(rec);
            Serial.printf("[Wiz] saved spool id=%s core=%d current=%d\n",
                          rec.id.c_str(), rec.weight_core, rec.weight_current);
            // Save path: hand the new spool id to main.cpp's callback so it
            // can arm PendingAms + show the spool-detail screen, matching
            // the existing-tag-rescan flow.
            close_wizard(rec.id);
            return;
        }
    }
}

static void build_done() {
    s_scr_done = lv_obj_create(nullptr);
    style_screen(s_scr_done);

    lv_obj_t* title = make_label(s_scr_done, "Review & save", COL_BRAND, &spoolhard_mont_22);
    lv_obj_set_pos(title, 16, 14);

    s_done_summary = make_label(s_scr_done, "", COL_TEXT, &spoolhard_mont_16);
    lv_obj_set_pos(s_done_summary, 16, 56);
    lv_obj_set_width(s_done_summary, 448);
    lv_label_set_long_mode(s_done_summary, LV_LABEL_LONG_WRAP);

    make_btn(s_scr_done, "Save",   true,  OP_DONE_SAVE,   on_done, 16,  236, 200);
    make_btn(s_scr_done, "Cancel", false, OP_DONE_CANCEL, on_done, 240, 236, 100);
    // Colour chip: filled with the spool colour, tap to change.
    s_done_color = make_btn(s_scr_done, "Color", false, OP_DONE_COLOR, on_done, 352, 236, 112);
    lv_obj_set_style_border_width(s_done_color, 3, 0);
}

static void show_done() {
    char buf[384];
    snprintf(buf, sizeof(buf),
        "Tag: %s\n"
        "Material: %s%s%s%s%s\n"
        "Advertised: %s   Core: %s   Current: %s",
        s_wiz.tag_uid.c_str(),
        s_wiz.final_brand.length()    ? s_wiz.final_brand.c_str()    : "—",
        s_wiz.final_material.length() ? "  " : "",
        s_wiz.final_material.c_str(),
        s_wiz.final_color_name.length() ? "  " : "",
        s_wiz.final_color_name.c_str(),
        s_wiz.final_advertised > 0 ? (String(s_wiz.final_advertised) + " g").c_str() : "—",
        s_wiz.final_core       > 0 ? (String(s_wiz.final_core)       + " g").c_str() : "—",
        s_wiz.final_current    >= 0 ? (String(s_wiz.final_current)   + " g").c_str() : "—");
    lv_lock();
    lv_label_set_text(s_done_summary, buf);
    uint32_t rgb = s_wiz.final_color_hex.length() == 6
                   ? strtoul(s_wiz.final_color_hex.c_str(), nullptr, 16) : 0xcccccc;
    lv_obj_set_style_bg_color(s_done_color, lv_color_hex(rgb), 0);
    lv_obj_t* cl = lv_obj_get_child(s_done_color, 0);
    lv_label_set_text(cl, s_wiz.final_color_name.length() ? s_wiz.final_color_name.c_str()
                                                          : "Color");
    lv_obj_set_style_text_color(cl, text_on(rgb), 0);
    lv_screen_load(s_scr_done);
    lv_unlock();
}

// ── Public API ─────────────────────────────────────────────

static bool s_built = false;

// First-tag-scan lazy initialisation of all wizard screens. Must hold
// lv_lock the whole time because every build_* call mutates the LVGL
// widget tree and setting style props dispatches LV_EVENT_STYLE_CHANGED
// through display observers that the LVGL task on Core 1 is walking
// concurrently. Running the builds on a naked Core-0 callback without
// the lock crashes with a null-pointer load inside lv_event_send — see
// the alpha-13 core dump traced to ui_wizard.cpp:119 / make_label.
static void build_all_once() {
    if (s_built) return;
    lv_lock();
    s_built = true;
    build_prompt();
    build_template();
    build_pick();
    build_state();
    build_full();
    build_used();
    build_usedlist();
    build_empty();
    build_color();
    build_done();
    lv_unlock();
}

// True when one of the wizard's own screens is on the LCD. Something
// outside the wizard (spool auto-close, OTA completion, ...) can load
// another screen without going through close_wizard(); the flag alone
// would then stay set forever and drop every later scan.
static bool wizard_screen_showing() {
    lv_lock();
    lv_obj_t* a = lv_screen_active();
    lv_unlock();
    return a && (a == s_scr_prompt   || a == s_scr_template || a == s_scr_pick  ||
                 a == s_scr_filapick || a == s_scr_state    || a == s_scr_full  ||
                 a == s_scr_used     || a == s_scr_empty    || a == s_scr_usedlist ||
                 a == s_scr_done     || a == s_scr_color    ||
                 a == s_scr_ftype    || a == s_scr_brand);
}

bool ui_wizard_start(const SpoolTag& tag) {
    if (s_wiz.active) {
        if (wizard_screen_showing()) {
            Serial.printf("[Wiz] drop tag %s — wizard already active\n", tag.uid_hex.c_str());
            return false;
        }
        Serial.printf("[Wiz] stale wizard (%s) lost its screen — restarting for %s\n",
                      s_wiz.tag_uid.c_str(), tag.uid_hex.c_str());
    }
    build_all_once();
    s_wiz            = WizardCtx{};
    s_wiz.active     = true;
    s_wiz.tag_uid    = tag.uid_hex;
    s_wiz.tag_format = tag.format;
    s_wiz.tag_type   = tag.tag_type;
    s_wiz.tag_url    = tag.ndef_url;
    s_wiz.hint_material  = tag.parsed_material;
    s_wiz.hint_brand     = tag.parsed_brand;
    s_wiz.hint_color_hex = tag.parsed_color_hex;
    s_pick_material_filter = "";
    show_prompt();
    return true;
}

bool ui_wizard_active() { return s_wiz.active && wizard_screen_showing(); }

void ui_wizard_on_weight(float grams, const char* state) {
    if (!s_wiz.active) return;
    s_last_grams = grams;
    s_last_state = state ? state : "";

    char buf[64];
    if (s_last_state == "removed")          snprintf(buf, sizeof(buf), "Scale: -- (empty)");
    else if (s_last_state == "uncalibrated") snprintf(buf, sizeof(buf), "Scale: uncalibrated");
    else if (s_last_state.length())          snprintf(buf, sizeof(buf), "Scale: %.0f g  (%s)", grams, s_last_state.c_str());
    else                                     snprintf(buf, sizeof(buf), "Scale: --");

    lv_lock();
    lv_obj_t* active = lv_screen_active();
    bool have_reading = s_last_grams > 0;
    if (active == s_scr_full)  {
        lv_label_set_text(s_lbl_full_weight, buf);
        // Capture needs both a weight AND a quick-weight choice; stability
        // is informational only (shown in the label).
        set_btn_enabled(s_btn_full_capture, have_reading && s_wiz.advertised_g > 0);
    }
    else if (active == s_scr_used) {
        lv_label_set_text(s_lbl_used_weight, buf);
        set_btn_enabled(s_btn_used_measure, have_reading);
        // Template shortcut tracks the same have-reading gate — both
        // measure the current filament mass as `measured − core`.
        if (s_btn_used_template && s_wiz.template_core > 0) {
            set_btn_enabled(s_btn_used_template, have_reading);
        }
    }
    else if (active == s_scr_empty) {
        lv_label_set_text(s_lbl_empty_weight, buf);
        set_btn_enabled(s_btn_empty_capture, have_reading);
    }
    lv_unlock();
}
