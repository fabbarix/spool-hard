#include "rgb_led.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>

static Adafruit_NeoPixel _pixel(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

void RgbLed::begin() {
    _pixel.begin();
    _pixel.setBrightness(40);
    off();
}

void RgbLed::update() {
    uint32_t now = millis();

    if (_mode == Mode::Flash) {
        if (now - _lastToggle >= _interval) {
            _lastToggle = now;
            _flashOn = !_flashOn;
            if (_flashOn) _write(_r, _g, _b);
            else          _write(0, 0, 0);
        }
    } else if (_mode == Mode::Pulse) {
        // Smooth sine-wave brightness: 0.15 → 1.0
        float phase = (float)(now % _interval) / (float)_interval;
        float brightness = 0.15f + 0.85f * (0.5f + 0.5f * sinf(phase * 2.0f * M_PI));
        _write((uint8_t)(_r * brightness),
               (uint8_t)(_g * brightness),
               (uint8_t)(_b * brightness));
    } else if (_mode == Mode::Burst) {
        if ((int32_t)(now - _burstNext) >= 0) {
            if (_burstPhaseOn) {
                // ON phase just ended → go dark for offMs.
                _write(0, 0, 0);
                _burstPhaseOn = false;
                _burstNext    = now + _burstOffMs;
            } else {
                // OFF phase just ended → one flash consumed. If any flashes
                // remain, start the next ON phase; otherwise the burst is
                // done and we drop back to Off so updateLed() picks up on
                // the next tick.
                if (--_burstRemaining == 0) {
                    _mode = Mode::Off;
                } else {
                    _write(_r, _g, _b);
                    _burstPhaseOn = true;
                    _burstNext    = now + _burstOnMs;
                }
            }
        }
    }
}

void RgbLed::solid(uint8_t r, uint8_t g, uint8_t b) {
    _mode = Mode::Solid;
    _r = r; _g = g; _b = b;
    _write(r, g, b);
}

void RgbLed::flash(uint8_t r, uint8_t g, uint8_t b, uint16_t intervalMs) {
    if (_mode == Mode::Flash && _r == r && _g == g && _b == b && _interval == intervalMs)
        return;
    _mode = Mode::Flash;
    _r = r; _g = g; _b = b;
    _interval = intervalMs;
    _lastToggle = millis();
    _flashOn = true;
    _write(r, g, b);
}

void RgbLed::pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t periodMs) {
    if (_mode == Mode::Pulse && _r == r && _g == g && _b == b && _interval == periodMs)
        return;
    _mode = Mode::Pulse;
    _r = r; _g = g; _b = b;
    _interval = periodMs;
}

void RgbLed::burst(uint8_t r, uint8_t g, uint8_t b,
                   uint8_t flashes, uint16_t onMs, uint16_t offMs) {
    if (flashes == 0) return;
    _mode            = Mode::Burst;
    _r = r; _g = g; _b = b;
    _burstOnMs       = onMs;
    _burstOffMs      = offMs;
    _burstRemaining  = flashes;
    _burstPhaseOn    = true;
    _burstNext       = millis() + onMs;
    _write(r, g, b);
}

void RgbLed::_write(uint8_t r, uint8_t g, uint8_t b) {
    _pixel.setPixelColor(0, _pixel.Color(r, g, b));
    _pixel.show();
}
