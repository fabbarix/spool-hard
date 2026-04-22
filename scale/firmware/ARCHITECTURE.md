# SpoolHard Scale — Firmware Architecture

## Hardware

**MCU:** Espressif ESP32-S3 (Xtensa LX7 dual-core, 240MHz, 16MB flash, PSRAM)

### Pin Map

| GPIO | Peripheral | Signal | Notes |
|------|-----------|--------|-------|
| 4 | HX711 | CLK | Load cell amplifier clock |
| 5 | HX711 | DATA | Load cell amplifier data |
| 8 | PN532 | IRQ | NFC interrupt request |
| 15 | PN532 | SCK | SPI clock |
| 16 | PN532 | MISO | SPI data out |
| 17 | PN532 | MOSI | SPI data in |
| 18 | PN532 | SS | SPI chip select |
| 48 | WS2812B | DATA | RGB status LED (single pixel) |
| 0 | Button | INPUT_PULLUP | Feature button (active LOW) |
| EN | Button | — | Hardware reset (not a GPIO) |

### Peripherals

**Load Cell (HX711)**
- 2-wire bit-bang protocol (DATA + CLK)
- Single-ended A channel, 128x gain
- Calibration stored in NVS: zero offset, reference weight, reference raw reading

**NFC Reader (PN532)**
- SPI mode (dip-switches set accordingly)
- Optional — firmware detects presence at boot and stores result in NVS
- Supports: read, write, erase, and emulate NTAG/Mifare Classic 1K/4K tags
- Reads Bambu Lab spool tags

**RGB LED (WS2812B)**
- Single pixel on GPIO 48 (ESP32-S3-DevKitC-1 built-in LED position)
- 3-channel RGB, no white channel
- Driven via Adafruit NeoPixel (RMT peripheral)
- Signal model: a "traffic-light" health axis (red → amber → green) overlaid
  with activity colours for specific operations. Priority runs highest to
  lowest; higher-priority states override lower ones while active.

| Priority | Pattern                 | Meaning                                                   |
|----------|-------------------------|-----------------------------------------------------------|
| 1        | amber slow pulse        | firmware / frontend update in progress                    |
| 2        | rapid green triple      | ephemeral — console confirmed a button "capture weight"   |
| 2        | rapid amber triple      | ephemeral — button capture rejected or no console reply   |
| 2        | bright-blue triple      | ephemeral — NFC tag read and parsed successfully          |
| 3        | dark-blue solid         | NFC tag read / write in flight (hold the tag steady)      |
| 4        | dark-teal flash         | load on scale, reading still settling                     |
| 4        | dark-teal solid         | load on scale, reading stable (ready to capture)          |
| 5        | green solid             | console paired, WiFi up — healthy idle                    |
| 6        | amber solid             | WiFi up, console not paired / offline                     |
| 7        | red flash               | provisioning AP active (no stored credentials)            |
| 8        | red solid               | offline — booting, connecting, or lost the WiFi link      |

**Feature Button (GPIO 0)**
- Active LOW, internal pull-up
- Short press → sends `ButtonPressed` event to console
- Long press → extended action
- Multiple rapid presses at boot → erases stored WiFi credentials and security key

---

## Firmware

**Framework:** Arduino (ESP-IDF underneath), C++17, PlatformIO

### Module Overview

| Module | File | Responsibility |
|--------|------|---------------|
| Load cell | `load_cell.cpp` | HX711 driver, weight state machine, NVS calibration |
| NFC reader | `nfc_reader.cpp` | PN532 SPI driver, NDEF read/write/erase/emulate |
| RGB LED | `rgb_led.cpp` | WS2812B single-pixel driver |
| Protocol | `protocol.cpp` | JSON serial framing, Scale↔Console message dispatch |
| Web server | `web_server.cpp` | AsyncWebServer, `/api/*` REST endpoints |
| OTA | `ota.cpp` | HTTP/HTTPS firmware update, configurable URL and SSL |
| Main | `main.cpp` | Setup, event loop, WiFi, state orchestration |

### Weight State Machine

```
Uncalibrated ──(calibrated)──► Idle
                                │
                         weight > 2g
                                │
                                ▼
                            NewLoad
                                │
                        samples stabilise
                                │
                                ▼
                           StableLoad ◄──────────────┐
                                │                    │
                         weight shifts          re-stabilises
                                │                    │
                                ▼                    │
                      LoadChangedUnstable ───────────┘
                                │
                         weight < 2g
                                │
                                ▼
                           LoadRemoved
                                │
                                ▼
                              Idle
```

### Console Protocol (Serial)

The scale communicates with the SpoolHard main unit over `Serial1` (115200 baud) using newline-delimited JSON.

**Scale → Console events:**

| Message | Payload | Trigger |
|---------|---------|---------|
| `Uncalibrated` | — | Boot without calibration data |
| `NewLoad` | `weight_g` | Object placed on scale |
| `LoadChangedStable` | `weight_g` | Weight shifted and re-stabilised |
| `LoadChangedUnstable` | `weight_g` | Weight shifting |
| `LoadRemoved` | — | Scale cleared |
| `RawSamplesAvg` | `weight_g`, `raw` | Every 500ms continuously |
| `ButtonPressed` | — | Feature button short press |
| `TagStatus` | `status`, `uid[]`, `url`, `is_bambulab` | NFC tag event |
| `PN532Status` | — | NFC module status change |
| `ScaleVersion` | `version` | Sent on console connect |
| `OtaProgress` | `percent` | During firmware update |
| `Pong` | — | Reply to Ping keepalive |

**Console → Scale commands:**

| Message | Payload | Action |
|---------|---------|--------|
| `Ping` | — | Keepalive; triggers `Pong` + version send on first connect |
| `SetTare` | — | Zero the scale at current reading |
| `CalibrateButton` | `weight` | Complete calibration with known weight |
| `WriteTag` | `uid[]`, `ndef_message`, `cookie` | Write NDEF to NFC tag |
| `EraseTag` | `uid[]` | Erase NFC tag |
| `EmulateTag` | `url` | Emulate NFC tag |
| `UpdateFirmware` | — | Trigger OTA update |
| `TagsInStore` | — | Update tag emulation pool |
| `RequestGcodeAnalysis` | `fetch_3mf`, `ip`, ... | Initiate G-code analysis |
| `GcodeAnalysisNotify` | — | G-code analysis notification |

### REST API (port 80)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/nfc-module-config` | Read NFC availability |
| POST | `/api/nfc-module-config` | Set NFC availability |
| GET | `/api/device-name-config` | Read device name |
| GET | `/api/ota-config` | Read OTA settings |
| POST | `/api/ota-config` | Write OTA settings (`url`, `use_ssl`, `verify_ssl`) |
| POST | `/api/reset-device` | Factory reset + reboot |
| GET | `/api/test-key` | Test security key |
| POST | `/api/fixed-key-config` | Set fixed security key |

### NVS Storage

| Namespace | Key | Type | Description |
|-----------|-----|------|-------------|
| `scale_cal` | `zero_lc` | long | Tare raw offset |
| `scale_cal` | `cal_weight` | float | Reference weight (grams) |
| `scale_cal` | `cal_lc` | long | Raw reading at reference weight |
| `nfc_cfg` | `available` | bool | PN532 detected at boot |
| `wifi_cfg` | `ssid` | string | WiFi SSID |
| `wifi_cfg` | `pass` | string | WiFi password |
| `wifi_cfg` | `device_name` | string | mDNS / SSDP device name |
| `ota_cfg` | `url` | string | Firmware binary URL |
| `ota_cfg` | `use_ssl` | bool | Use HTTPS (default: true) |
| `ota_cfg` | `verify_ssl` | bool | Verify TLS certificate (default: true) |

### OTA

- URL, SSL usage, and certificate verification are runtime-configurable via `/api/ota-config`
- Uses `HTTPUpdate` with `WiFiClientSecure` (HTTPS) or `WiFiClient` (HTTP)
- Progress reported to console in 0–100% increments
- Reboots automatically on success; LED turns red on failure
- Default URL: `https://device.spoolhard.io/bins/0.6/scale/ota/firmware.bin`
