#pragma once
#include <Arduino.h>

class RgbLed {
public:
    void begin();
    void update();

    void solid(uint8_t r, uint8_t g, uint8_t b);
    void off() { solid(0, 0, 0); _mode = Mode::Off; }
    void flash(uint8_t r, uint8_t g, uint8_t b, uint16_t intervalMs = 300);
    void pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t periodMs = 2000);

    /// Fire a short acknowledgement pattern: `flashes` ON phases of `onMs`
    /// separated by `offMs` gaps, ending with the LED off. Self-clears back
    /// to Mode::Off when done; callers should suppress steady-state LED
    /// updates (via isBusy()) so they don't overwrite the burst mid-play.
    void burst(uint8_t r, uint8_t g, uint8_t b,
               uint8_t flashes = 2, uint16_t onMs = 110, uint16_t offMs = 90);

    /// Triple bright-blue flash — same hue family as the dark-blue
    /// NFC-activity solid, so the user reads it as "tag-related, success".
    /// 3 pulses x ~100 ms keeps the ack snappy without delaying the next
    /// action.
    void ackTagRead()     { burst(0, 80, 255, 3, 100, 80); }

    /// Rapid pure-green triple flash — the console confirmed the user's
    /// scale-button "capture current weight" landed on an active spool.
    /// Distinct from ackTagRead (twin) so the user can tell a tag-read
    /// from a weight-capture at a glance. No blue channel so it can't
    /// read as teal at a glance.
    void ackCaptureOk()   { burst(0, 180, 0, 3, 100, 80); }
    /// Rapid amber triple flash — capture attempted but the console either
    /// rejected it (no active spool / no stable weight / context expired)
    /// or never responded within the wait window.
    void ackCaptureFail() { burst(200, 90,  0, 3, 100, 80); }

    /// True while a burst is playing. Steady-state drivers should skip their
    /// work for a few ticks while this is set.
    bool isBusy() const { return _mode == Mode::Burst; }

    // ── Semantic states ──────────────────────────────────────
    //
    // Priority order, enforced by updateLed() in main.cpp (highest first):
    //   1. showUpdating()          — OTA / frontend upload in progress
    //   2. transient burst playing — ack* above
    //   3. showNfcActivity()       — tag read/write in flight
    //   4. showWeightStable()      — stable load on scale
    //   5. showConsoleConnected()  — console paired, everything green
    //   6. showWifiOnly()          — on WiFi, console absent
    //   7. showApMode()            — no credentials, provisioning portal up
    //   8. showOffline()           — booting, connecting, dropped link
    //
    // Every non-transient state is a single solid colour or a single
    // tempo at a single hue — the user only has to recognise the colour,
    // not disambiguate shades.
    void showOffline()          { solid(255, 0,   0); }          // red solid
    void showApMode()           { flash(255, 0,   0,  400); }    // red flash
    void showWifiOnly()         { solid(200, 90,  0); }         // amber solid
    // Pure, dimmed green — no blue channel, low intensity. Needs to stay
    // visibly distinct from teal on a diffused WS2812; earlier
    // (0,200,60) was too bright and its 60 of blue blurred into teal.
    void showConsoleConnected() { solid(0,   120, 0); }          // green solid (dim)
    // Dark teal for both weight states. Flashing while a load is on the
    // scale but still settling, solid once the reading has stabilised —
    // same hue so the user sees it as "weight in progress → weight done"
    // rather than two unrelated states.
    void showWeightUnstable()   { flash(20,  100, 90, 250); }    // dark-teal flash
    void showWeightStable()     { solid(20,  100, 90); }         // dark-teal solid
    // Dark blue for both NFC states. Solid while the read/write is in
    // flight ("hold the tag steady"), then a triple bright-blue burst from
    // ackTagRead() to confirm a successful parse.
    void showNfcActivity()      { solid(0,   40,  120); }        // dark-blue solid
    void showUpdating()         { pulse(200, 90,  0, 2000); }   // amber slow pulse

private:
    enum class Mode { Off, Solid, Flash, Pulse, Burst };

    Mode     _mode       = Mode::Off;
    uint8_t  _r = 0, _g = 0, _b = 0;
    uint16_t _interval   = 300;
    uint32_t _lastToggle = 0;
    bool     _flashOn    = true;

    // Burst state
    uint8_t  _burstRemaining = 0;
    uint16_t _burstOnMs      = 0;
    uint16_t _burstOffMs     = 0;
    uint32_t _burstNext      = 0;
    bool     _burstPhaseOn   = false;

    void _write(uint8_t r, uint8_t g, uint8_t b);
};
