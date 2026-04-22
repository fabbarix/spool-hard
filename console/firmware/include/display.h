#pragma once
#include <stdint.h>

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
};
