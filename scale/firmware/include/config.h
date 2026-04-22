#pragma once

// ── Product identity ─────────────────────────────────────────
// PRODUCT_ID, PRODUCT_NAME and OTA_DEFAULT_URL are now set in
// platformio.ini build_flags so the shared spoolhard_core library
// (which doesn't see this header) can pick them up via -D macros.

// ── HX711 pins ──────────────────────────────────────────────
#define HX711_DATA_PIN   5
#define HX711_CLK_PIN    4

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
#define WEIGHT_SAMPLES       10
#define STABLE_THRESHOLD_G   1.0f
#define STABLE_COUNT_REQ     5
#define LOAD_DETECT_G        2.0f

#define NVS_NS_SCALE         "scale_cfg"
#define NVS_KEY_SAMPLES      "samples"
#define NVS_KEY_STABLE_THR   "stable_thr"
#define NVS_KEY_STABLE_CNT   "stable_cnt"
#define NVS_KEY_LOAD_DET     "load_det"
#define NVS_KEY_PRECISION    "precision"
#define NVS_KEY_ROUNDING     "rounding"
