#pragma once
#include <stdint.h>
#include <stddef.h>
#include <memory>

struct SnapStream;   // opaque per-stream snapshot ownership (display.cpp)

// Brings up the WT32-SC01-Plus LCD + touch via LovyanGFX, wires it into
// LVGL 9, and spawns an LVGL task pinned to Core 1. After begin() returns,
// lv_* APIs are safe to call from any core as long as they are wrapped in
// lv_lock()/lv_unlock() (LV_USE_OS = FREERTOS).
//
// Also owns the screen-sleep state machine: when no touch has been observed
// for `sleep_timeout_s` seconds the backlight PWM drops to 0; the next touch
// turns it back on and is swallowed so the wake tap doesn't register as a
// click. A timeout of 0 disables the feature (screen always on).
class ConsoleDisplay {
public:
    void begin();

    // Setting of 0 disables sleep (always on). Any change refreshes the idle
    // counter so the new window starts from "now" — and wakes the screen if
    // it was asleep. Safe to call from any core.
    static void     setSleepTimeout(uint32_t seconds);
    static uint32_t sleepTimeout();       // current setting, seconds

    // Clamp to [0, 3600], persist to NVS, and apply immediately. The single
    // source of truth shared by the web `/api/display-config` POST handler
    // and the on-device config screen, so the clamp bound + NVS keys can't
    // drift between the two write paths.
    static void setAndPersistSleepTimeout(uint32_t seconds);

    // Force the screen back on and reset the idle counter. Useful when the
    // firmware itself wants to surface something to the user (OTA progress,
    // error screen, etc.) AND it represents a bounded event the user should
    // have a fresh look-at window for.
    static void wake();

    // Wake the screen if it's currently asleep, but do NOT reset the idle
    // counter if it was already awake. Use this for autonomous events that
    // can fire repeatedly on their own schedule — e.g. scale weight pushes
    // while a load drifts, or periodic sensor updates. Without this split,
    // a continuously-reporting subsystem would pin the display on forever
    // by refreshing the timer faster than it can expire.
    static void wakeIfAsleep();

    static bool isAsleep();

    // ── Debug LCD access (gated by the web-layer debug toggle) ──────────
    // Snapshot: the screen is rendered by a one-shot task into a single
    // PSRAM buffer (RGB565, ~300 KB). When a client claims it
    // (snapshotClaim) ownership moves into a SnapStream held by the chunked
    // response's fill lambda, so the buffer lives exactly as long as that
    // response (freed when the response is destroyed: end of body, client
    // disconnect, or lib timeout) and nothing else can free it mid-stream.
    // An unclaimed READY buffer is swept after 30 s / on toggle-off.
    // All request-side calls run on the AsyncTCP task (serialised).
    enum SnapStatus : uint8_t { SNAP_PENDING, SNAP_READY, SNAP_BUSY, SNAP_FAILED };
    static constexpr size_t SNAP_BMP_BYTES = 54 + 480 * 320 * 3;
    // IDLE: starts a capture -> PENDING. PENDING: still rendering.
    // READY: a buffer is waiting; call snapshotClaim(). BUSY: a stream is
    // in progress. FAILED: capture failed.
    static SnapStatus snapshotRequest();
    // Only valid right after snapshotRequest() returned SNAP_READY.
    static std::shared_ptr<SnapStream> snapshotClaim();
    // Chunked-response fill callback; pure function of `index` (idempotent
    // under lib retries); returns 0 only at the true end of the body.
    static size_t snapshotFill(const SnapStream& st, uint8_t* out, size_t maxLen, size_t index);
    // Free an unclaimed READY buffer now (debug toggle turned off).
    static void       snapshotRelease();
    // Frees an unclaimed READY buffer older than 30 s; called from the
    // LVGL task loop. Never touches a stream in progress.
    static void       snapshotSweep();

    // Touch injection into the LVGL indev. action: 0 = tap (press, ~100 ms,
    // release), 1 = press (held until release; auto-released after 5 s),
    // 2 = release. Returns false if a previous injection is still pending
    // (caller replies 409). Coordinates must already be validated.
    static bool injectTouch(uint16_t x, uint16_t y, uint8_t action);
};
