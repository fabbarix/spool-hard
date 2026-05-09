#pragma once

// ── Product identity ─────────────────────────────────────────
// PRODUCT_ID, PRODUCT_NAME and OTA_DEFAULT_URL are now set in
// platformio.ini build_flags so the shared spoolhard_core library
// (which doesn't see this header) can pick them up via -D macros.

// ── HX711 pins ──────────────────────────────────────────────
#define HX711_DATA_PIN   5
#define HX711_CLK_PIN    4

// HX711 sample-rate mode. The chip's RATE pin is hardware-wired:
//   - tied LOW  → 10 Hz output (default Bogde library expectation)
//   - tied HIGH → 80 Hz output
// On Sparkfun-style breakouts there's a solder jumper labelled "RATE".
// On bare HX711 modules pin 15 of the IC sets it. We can't switch it
// from firmware — the constant below tells `sensor_task` how often to
// poll. At 80 Hz the chip drops a fresh sample every ~12 ms; the
// poll-tick must be < 12 ms or we skip samples (the chip latches the
// next sample and the previous one is lost on the next CK pulse).
//
// Set to `80` for 80 Hz hardware (snappier stable detection — 5×
// faster convergence on stable-count). The poll loop in sensor_task
// already runs at 100 Hz so the higher rate is absorbed without
// further changes.
#define HX711_HW_RATE_HZ   80

// ── PN532 SPI pins ───────────────────────────────────────────
#define PN532_SCK_PIN    15
#define PN532_MISO_PIN   16
#define PN532_MOSI_PIN   17
#define PN532_SS_PIN     18
#define PN532_IRQ_PIN    8

// ── RGB LED ──────────────────────────────────────────────────
#define RGB_LED_PIN      48

// ── Button ───────────────────────────────────────────────────
#define BUTTON_PIN       0   // GPIO0 — confirmed by hardware test
                             // Second button is RST (EN pin), not a GPIO

// ── Serial console (to SpoolHard main unit) ─────────────────
#define CONSOLE_SERIAL   Serial1
#define CONSOLE_BAUD     115200

// ── NVS keys ─────────────────────────────────────────────────
#define NVS_NS_CALIBRATION   "scale_cal"
#define NVS_KEY_ZERO         "zero_lc"
#define NVS_KEY_CALIB_WEIGHT "cal_weight"
#define NVS_KEY_CALIB_LC     "cal_lc"
#define NVS_NS_NFC           "nfc_cfg"
#define NVS_KEY_NFC_AVAIL    "available"
#define NVS_NS_WIFI          "wifi_cfg"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "pass"
#define NVS_KEY_DEVICE_NAME  "device_name"
#define NVS_KEY_FIXED_KEY    "fixed_key"
#define DEFAULT_FIXED_KEY    "Change-Me!"
// Optional mesh-AP pin: "AA:BB:CC:DD:EE:FF" or "" for auto-select.
// When set, WiFi.begin() targets this exact BSSID. 60 s fallback in
// wifi_provisioning's update() drops the pin in RAM if the targeted
// node is offline, so the device stays reachable.
#define NVS_KEY_PINNED_BSSID "pinned_bssid"

// ── OTA ──────────────────────────────────────────────────────
// FW_VERSION / FE_VERSION are populated from scale/VERSION at build time by
// scripts/patch_version.py. Fallbacks below kick in only for ad-hoc builds
// that bypass the pre-script (e.g. a direct `gcc`).
#ifndef FW_VERSION
  #define FW_VERSION "dev"
#endif
#ifndef FE_VERSION
  #define FE_VERSION "dev"
#endif

// OTA persistence (namespace + keys) is owned by spoolhard_core/src/ota.cpp
// — see the local-namespace constants there. Both products use the same
// schema; the macros are not duplicated here to avoid drift.

// ── Weight sampling (defaults — overridden by NVS at runtime) ─
//
// `STABLE_COUNT_REQ` is "samples within `STABLE_THRESHOLD_G` before
// the state machine declares Stable". At 10 Hz hardware mode the old
// default (5 samples ≈ 500 ms) felt fine; at 80 Hz those same 5
// samples would fire after 60 ms of motionlessness, false-positiving
// the user. Bumped to 40 (≈ 500 ms wall-clock at 80 Hz) so the UX
// is unchanged regardless of hardware rate. NVS-overridable per unit.
#define WEIGHT_SAMPLES       20
#define STABLE_THRESHOLD_G   1.0f
#define STABLE_COUNT_REQ     40
#define LOAD_DETECT_G        2.0f

#define NVS_NS_SCALE         "scale_cfg"
#define NVS_KEY_SAMPLES      "samples"
#define NVS_KEY_STABLE_THR   "stable_thr"
#define NVS_KEY_STABLE_CNT   "stable_cnt"
#define NVS_KEY_LOAD_DET     "load_det"
#define NVS_KEY_PRECISION    "precision"
#define NVS_KEY_ROUNDING     "rounding"
