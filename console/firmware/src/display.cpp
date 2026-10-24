#include "display.h"
#include "config.h"

#include <Arduino.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <atomic>
#include "spoolhard/serial_mirror.h"

// ── LovyanGFX board definition for the WT32-SC01-Plus ────────
// Pin assignments are from the WT32-SC01-Plus vendor schematic.
// LCD: 8-bit parallel bus on LCD_CAM peripheral.
// Touch: FT5x06 capacitive controller over I²C0.

class LGFX_Console : public lgfx::LGFX_Device {
    // ST7796 controller in BGR mode per the reference
    // (https://github.com/Cesarbautista10/WT32-SC01-Plus-ESP32).
    lgfx::Panel_ST7796   _panel;
    lgfx::Bus_Parallel8  _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_FT5x06   _touch;   // LovyanGFX class handles FT5x06 / FT6336U family

public:
    LGFX_Console() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000;
            cfg.pin_wr     = 47;
            cfg.pin_rd     = -1;
            cfg.pin_rs     = 0;   // D/C
            cfg.pin_d0 = 9;   cfg.pin_d1 = 46;  cfg.pin_d2 = 3;   cfg.pin_d3 = 8;
            cfg.pin_d4 = 18;  cfg.pin_d5 = 17;  cfg.pin_d6 = 16;  cfg.pin_d7 = 15;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs          = -1;
            cfg.pin_rst         = 4;
            cfg.pin_busy        = -1;
            cfg.panel_width     = 320;
            cfg.panel_height    = 480;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = true;   // ST7796_INVON per reference
            // LVGL 9 emits RGB565 as RGB-order bits (R in the top 5). Setting
            // `rgb_order = true` would make the ST7796 decode those bits as
            // BGR, so every pixel's R and B channels get swapped at draw
            // time — yellow shows up as cyan/blue, brand amber as sky-blue.
            // The reference datasheet sets the MADCTL BGR bit but their
            // framebuffer was already in BGR order; ours isn't. Force RGB.
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 45;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        {
            auto cfg = _touch.config();
            cfg.x_min           = 0;
            cfg.x_max           = 319;
            cfg.y_min           = 0;
            cfg.y_max           = 479;
            cfg.pin_int         = -1;
            cfg.bus_shared      = false;
            // Touch vs panel orientation: the earlier "offset_rotation = 2"
            // produced a 180°-mirrored touch surface (tap bottom-right,
            // reported top-left). Nobody noticed until the first on-screen
            // buttons shipped — previous screens were display-only. Setting
            // this to 0 aligns touch with the panel's rotation=1 landscape.
            cfg.offset_rotation = 0;
            cfg.i2c_port        = 0;
            cfg.i2c_addr        = 0x38;    // FT5x06
            cfg.pin_sda         = 6;
            cfg.pin_scl         = 5;
            cfg.freq            = 400000;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};

static LGFX_Console lcd;
static lv_display_t* s_disp  = nullptr;
static lv_indev_t*   s_indev = nullptr;

// ── Screen-sleep state ──────────────────────────────────────
// Both the LVGL task (core 1, reads on every tick) and the web-API handler
// (core 0, writes via setSleepTimeout) touch these, so flag them volatile.
// The individual operations are single word-sized writes so we don't need a
// mutex — the worst case is one tick of wrong backlight state.
static constexpr uint8_t BACKLIGHT_ON  = 200;   // matches the init brightness
static constexpr uint8_t BACKLIGHT_OFF = 0;
static volatile uint32_t s_last_interaction_ms = 0;
static volatile uint32_t s_sleep_timeout_ms    = 0;   // 0 = disabled
static volatile bool     s_display_asleep      = false;

static void apply_sleep() {
    if (s_display_asleep) return;
    s_display_asleep = true;
    lcd.setBrightness(BACKLIGHT_OFF);
}
static void apply_wake() {
    if (!s_display_asleep) return;
    s_display_asleep = false;
    lcd.setBrightness(BACKLIGHT_ON);
}

// Partial-render draw buffers — 1/8 of a 480x320 framebuffer each
// (~38 KiB per buffer, ~75 KiB total). Allocated from PSRAM rather than
// parked in .bss so the internal-DRAM budget stays intact — mbedtls needs
// ~30 KiB of contiguous internal DRAM for a TLS handshake against the
// printer's FTPS server, which was failing because these buffers had
// carved the address space up. LVGL's CPU-side flush copies out before
// the LCD bus write so the PSRAM latency doesn't show up as frame jitter.
//
// LVGL 9 asserts that the buffer start is aligned to its `lv_draw_buf_align`
// (4 bytes for RGB565 with the default config, potentially higher with
// certain SIMD paths). 16-byte alignment is conservative for any config.
static constexpr uint32_t DRAW_BUF_PIXELS = 480 * 40;
static uint16_t* s_buf1 = nullptr;
static uint16_t* s_buf2 = nullptr;

static void flush_cb(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    // LVGL 9 writes RGB565 in host (LE) byte order; the ST7796 wants big-endian
    // on the 8-bit parallel bus. `pushPixels(data, len, swap=true)` performs
    // that byte swap — `writePixels(data, len, dma)` does NOT (the third arg
    // there is use_dma, not swap, which is why colors looked pink before).
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.pushPixels((uint16_t*)px_map, w * h, true);
    lcd.endWrite();
    lv_display_flush_ready(d);
}

// Tracks edge-transition of the pressed state so we can log one line per
// press and one per release, instead of spamming while LVGL polls us at
// ~200 Hz. File-scope static — single input device so no reentrancy issue.
static bool s_touch_was_pressed = false;

// Set when a touch wakes the screen. While true every subsequent press is
// reported to LVGL as RELEASED so the *entire* wake gesture (press → hold →
// lift) is masked — not just the first sample. Cleared on the first sample
// where the finger is no longer pressed, so the NEXT tap interacts normally.
// Without this, the first read reported RELEASED but the remaining reads
// before lift-off reported PRESSED, which LVGL then dispatched as a click
// on lift — exactly the behaviour the swallow was meant to prevent.
static bool s_swallow_until_release = false;

static bool inj_poll(uint16_t* x, uint16_t* y);   // debug touch injection, below

static void touch_cb(lv_indev_t*, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed = lcd.getTouch(&x, &y);
    bool injected = false;
    if (!pressed && inj_poll(&x, &y)) { pressed = true; injected = true; }
    if (pressed) {
        // If the screen was asleep, arm the full-gesture swallow — the user
        // tapped to wake, not to interact with whatever happened to be
        // under their finger. apply_wake() is idempotent so re-entering
        // while the swallow is still armed is harmless.
        if (s_display_asleep) {
            apply_wake();
            // Injected (debug) touches wake the screen but are not swallowed.
            if (!injected) s_swallow_until_release = true;
        }
        s_last_interaction_ms = millis();
        if (s_swallow_until_release) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        if (!s_touch_was_pressed) {
            Serial.printf("[Touch] press @ %u,%u\n", x, y);
            s_touch_was_pressed = true;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        // Finger lifted — clear the swallow so the next tap is a real one.
        s_swallow_until_release = false;
        if (s_touch_was_pressed) {
            Serial.println("[Touch] release");
            s_touch_was_pressed = false;
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
}


// ── Debug touch injection ───────────────────────────────────
// One packed command word (bit31 valid | action<<20 | y<<10 | x), written
// by the web handler (compare-exchange from 0, so a second injection while
// one is unconsumed is refused) and consumed by touch_cb on the LVGL task.
// No heap, no queue. Zero cost when unused: a single relaxed atomic load.
static std::atomic<uint32_t> s_inj_cmd{0};
static constexpr uint32_t INJ_TAP_MS  = 100;
static constexpr uint32_t INJ_HOLD_MAX_MS = 5000;

bool ConsoleDisplay::injectTouch(uint16_t x, uint16_t y, uint8_t action) {
    uint32_t want = (1u << 31) | ((uint32_t)(action & 3) << 20) | ((uint32_t)y << 10) | x;
    uint32_t expected = 0;
    return s_inj_cmd.compare_exchange_strong(expected, want);
}

// Called only from touch_cb (LVGL task). Returns true while an injected
// finger is down; fills x/y.
static bool inj_poll(uint16_t* x, uint16_t* y) {
    static bool held = false;
    static uint16_t hx = 0, hy = 0;
    static uint32_t release_at = 0;
    uint32_t cmd = s_inj_cmd.load(std::memory_order_relaxed);
    uint32_t now = millis();
    if (!held) {
        if (!cmd) return false;
        uint8_t action = (cmd >> 20) & 3;
        s_inj_cmd.store(0);
        if (action == 2) return false;           // release with nothing held
        held = true;
        hx = cmd & 0x3FF; hy = (cmd >> 10) & 0x3FF;
        release_at = now + (action == 0 ? INJ_TAP_MS : INJ_HOLD_MAX_MS);
    } else {
        if (cmd && ((cmd >> 20) & 3) == 2) { s_inj_cmd.store(0); held = false; return false; }
        if ((int32_t)(now - release_at) >= 0) { held = false; return false; }
    }
    *x = hx; *y = hy;
    return true;
}

// ── Debug LCD snapshot ──────────────────────────────────────
// IDLE -> CAPTURING (task) -> READY -> STREAMING -> IDLE. All transitions
// except CAPTURING->READY/FAILED happen on the AsyncTCP task.
enum : uint8_t { SN_IDLE, SN_CAPTURING, SN_READY, SN_STREAMING, SN_FAILED };
static std::atomic<uint8_t> s_snap_state{SN_IDLE};
static lv_draw_buf_t*       s_snap_buf = nullptr;
static volatile uint32_t    s_snap_ts  = 0;
static std::atomic<uint32_t> s_snap_gen{0};   // bumped per claimed stream
static constexpr uint32_t   SNAP_STALE_MS = 30000;

static void snap_free() {
    if (s_snap_buf) { lv_draw_buf_destroy(s_snap_buf); s_snap_buf = nullptr; }
    s_snap_state.store(SN_IDLE);
}

static void snap_task(void*) {
    lv_lock();
    lv_draw_buf_t* b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lv_unlock();
    if (b && (b->header.w != 480 || b->header.h != 320)) { lv_draw_buf_destroy(b); b = nullptr; }
    s_snap_buf = b;
    s_snap_ts = millis();
    s_snap_state.store(b ? SN_READY : SN_FAILED);
    vTaskDelete(nullptr);
}

ConsoleDisplay::SnapStatus ConsoleDisplay::snapshotRequest() {
    switch (s_snap_state.load()) {
    case SN_CAPTURING: return SNAP_PENDING;
    case SN_FAILED:    s_snap_state.store(SN_IDLE); return SNAP_FAILED;
    case SN_READY:
        if (millis() - s_snap_ts < SNAP_STALE_MS) {
            s_snap_ts = millis();
            s_snap_gen.fetch_add(1);
            s_snap_state.store(SN_STREAMING);
            return SNAP_READY;
        }
        snap_free();
        break;                                   // stale: recapture below
    case SN_STREAMING:
        if (millis() - s_snap_ts < SNAP_STALE_MS) return SNAP_BUSY;
        snap_free();                             // abandoned stream
        break;
    default: break;
    }
    s_snap_state.store(SN_CAPTURING);
    if (xTaskCreatePinnedToCore(snap_task, "lcd_snap", 8192, nullptr, 1, nullptr, 1) != pdPASS) {
        s_snap_state.store(SN_IDLE);
        return SNAP_FAILED;
    }
    return SNAP_PENDING;
}

void ConsoleDisplay::snapshotAbort(uint32_t gen) {
    if (gen == s_snap_gen.load() && s_snap_state.load() == SN_STREAMING) snap_free();
}

uint32_t ConsoleDisplay::snapshotGeneration() { return s_snap_gen.load(); }

void ConsoleDisplay::snapshotRelease() {
    uint8_t st = s_snap_state.load();
    if (st == SN_READY || st == SN_STREAMING) snap_free();
}

void ConsoleDisplay::snapshotSweep() {
    uint8_t st = s_snap_state.load();
    if ((st == SN_READY || st == SN_STREAMING) && millis() - s_snap_ts >= SNAP_STALE_MS) snap_free();
}

size_t ConsoleDisplay::snapshotFill(uint8_t* out, size_t maxLen, size_t index) {
    if (s_snap_state.load() != SN_STREAMING || !s_snap_buf) return 0;
    if (index >= SNAP_BMP_BYTES) { snap_free(); return 0; }
    s_snap_ts = millis();
    size_t n = 0;
    if (index < 54) {
        uint8_t h[54] = {0};
        auto le32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (v >> (8 * i)) & 0xFF; };
        h[0] = 'B'; h[1] = 'M';
        le32(2, SNAP_BMP_BYTES); le32(10, 54); le32(14, 40);
        le32(18, 480); le32(22, (uint32_t)(int32_t)-320);   // negative = top-down
        h[26] = 1; h[28] = 24;
        le32(34, 480 * 320 * 3);
        n = 54 - index; if (n > maxLen) n = maxLen;
        memcpy(out, h + index, n);
        index += n;
    }
    const uint8_t* base = s_snap_buf->data;
    const uint32_t stride = s_snap_buf->header.stride;
    while (n < maxLen && index < SNAP_BMP_BYTES) {
        size_t p = index - 54;
        size_t px = p / 3; int c = p % 3;
        uint32_t row = px / 480, col = px % 480;
        const uint8_t* q = base + row * stride + col * 2;
        uint16_t v = q[0] | (q[1] << 8);
        uint8_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        uint8_t comp[3] = { (uint8_t)((b << 3) | (b >> 2)), (uint8_t)((g << 2) | (g >> 4)), (uint8_t)((r << 3) | (r >> 2)) };
        for (; c < 3 && n < maxLen; c++) { out[n++] = comp[c]; index++; }
    }
    return n;
}

static uint32_t tick_cb() { return millis(); }

static void lvgl_task(void*) {
    for (;;) {
        ConsoleDisplay::snapshotSweep();
        lv_lock();
        uint32_t t = lv_timer_handler();
        lv_unlock();
        // Idle-to-sleep check. Does not touch LVGL state so it's fine outside
        // the lock. Skipped when the timeout is 0 (feature disabled) or when
        // we're already asleep (no work to do).
        if (!s_display_asleep && s_sleep_timeout_ms > 0) {
            uint32_t since = millis() - s_last_interaction_ms;
            if (since >= s_sleep_timeout_ms) apply_sleep();
        }
        // Follow lv_timer_handler()'s hint (ms until the next timer is
        // due; LV_NO_TIMER_READY when none). Clamp to 5..30 ms while awake
        // so touch polling stays responsive; asleep there's nothing to
        // animate, but still poll touch (wake-on-tap) at ~50 ms.
        uint32_t d = t;
        if (d < 5) d = 5;
        uint32_t maxd = s_display_asleep ? 50 : 30;
        if (d > maxd) d = maxd;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

void ConsoleDisplay::begin() {
    Serial.println("[Display] Starting LovyanGFX + LVGL");
    lcd.init();
    lcd.setRotation(1);      // landscape 480×320 (MADCTL 0x28 = MV | BGR)
    lcd.setSwapBytes(true);
    lcd.setBrightness(200);
    lcd.fillScreen(TFT_BLACK);

    lv_init();
    lv_tick_set_cb(tick_cb);

    // Allocate the two draw buffers in PSRAM (see declaration comment).
    // 16-byte aligned to satisfy LVGL 9's lv_draw_buf_align assert. If
    // PSRAM alloc fails for any reason, fall back to internal DRAM so
    // the device at least boots — an unhappy LCD is better than no UI.
    const size_t buf_bytes = DRAW_BUF_PIXELS * sizeof(uint16_t);
    s_buf1 = (uint16_t*)heap_caps_aligned_alloc(16, buf_bytes,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf2 = (uint16_t*)heap_caps_aligned_alloc(16, buf_bytes,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf1 || !s_buf2) {
        Serial.println("[Display] PSRAM draw-buffer alloc failed, falling back to DRAM");
        if (!s_buf1) s_buf1 = (uint16_t*)heap_caps_aligned_alloc(16, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_buf2) s_buf2 = (uint16_t*)heap_caps_aligned_alloc(16, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    s_disp = lv_display_create(480, 320);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2,
                           buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_cb);

    s_last_interaction_ms = millis();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 1, nullptr, 1);
}

// ── Public sleep API ─────────────────────────────────────────

void ConsoleDisplay::setSleepTimeout(uint32_t seconds) {
    s_sleep_timeout_ms = (uint32_t)seconds * 1000u;
    // Restart the idle window from "now" and wake the screen if it's off —
    // either the user just changed the setting (and deserves to see the UI
    // come back), or the firmware is disabling sleep entirely.
    s_last_interaction_ms = millis();
    apply_wake();
}

uint32_t ConsoleDisplay::sleepTimeout() { return s_sleep_timeout_ms / 1000u; }

void ConsoleDisplay::setAndPersistSleepTimeout(uint32_t seconds) {
    // Clamp upper bound — more than an hour idle makes the feature useless
    // and would mask a forgotten "never sleep". Mirrors the bound the web
    // handler used to enforce inline before this became shared.
    if (seconds > 3600) seconds = 3600;
    Preferences p;
    p.begin(NVS_NS_DISPLAY, false);
    p.putUInt(NVS_KEY_DISP_SLEEP_S, seconds);
    p.end();
    setSleepTimeout(seconds);   // applies immediately + wakes screen
}

void ConsoleDisplay::wake() {
    s_last_interaction_ms = millis();
    apply_wake();
}

void ConsoleDisplay::wakeIfAsleep() {
    // Only touch the idle counter when we actually transitioned from
    // asleep→awake — otherwise a subsystem that pushes events faster
    // than the sleep timeout (e.g. a scale reporting an unstable load
    // every ~100 ms) would reset the timer on every tick and the
    // display would never sleep even after the user stopped interacting.
    if (s_display_asleep) {
        s_last_interaction_ms = millis();
        apply_wake();
    }
}

bool ConsoleDisplay::isAsleep() { return s_display_asleep; }
