# SpoolHard Console — Firmware Architecture

## Hardware

**MCU:** Espressif ESP32-S3 on the WT32-SC01-Plus board (16 MB flash, 2 MB PSRAM, QIO/OPI).

### Pin map

| GPIO | Peripheral | Signal | Notes |
|------|-----------|--------|-------|
| 0 | LCD | RS / DC | Data-command on the 8-bit parallel bus |
| 3, 8, 9, 15–18, 46 | LCD | D0–D7 | 8-bit parallel data (order: D0=9, D1=46, D2=3, D3=8, D4=18, D5=17, D6=16, D7=15) |
| 4 | LCD | RESET | |
| 47 | LCD | WR | Write strobe |
| 45 | Backlight | PWM | LEDC channel 7, 44.1 kHz |
| 5 | Touch FT5x06 | SCL | I²C |
| 6 | Touch FT5x06 | SDA | I²C, address 0x38 |
| 10 | PN532 | SS | Extended header pin 3 (EXT_IO1) |
| 11 | PN532 | MOSI | Extended header pin 4 (EXT_IO2) |
| 12 | PN532 | MISO | Extended header pin 5 (EXT_IO3) |
| 13 | PN532 | SCK | Extended header pin 6 (EXT_IO4) |
| 14 | PN532 | IRQ | Extended header pin 7 (EXT_IO5) |

No microSD, no external LEDs, no discrete buttons — all input is via the touch screen.

### Peripherals

**LCD + Touch (LovyanGFX)**
- ST7796-family panel, 480×320, landscape rotation 3.
- 8-bit parallel on ESP32-S3 LCD_CAM at 20 MHz.
- Capacitive touch via FT5x06 on I²C0 at 400 kHz.

**NFC reader (PN532)**
- SPI on a dedicated bus (HSPI / SPI2_HOST).
- `Adafruit_PN532` driver; low passive-activation retries so polling is non-blocking.
- Supports SpoolHard V1/V2 NDEF-URI tags; Bambu Lab proprietary + OpenPrintTag
  parsers are M3 work.

## Firmware

**Framework:** Arduino (ESP-IDF underneath), C++17, PlatformIO.

### Task/core layout

ESP32-S3 has two Xtensa cores. Tasks are partitioned so heavy UI work and
heavy networking don't contend:

- **Core 0** (default Arduino loop task + AsyncTCP callbacks): WiFi,
  `AsyncWebServer`, SSDP listener, WebSocket client to the scale, PN532 poll,
  OTA download. `setup()` and `loop()` run here.
- **Core 1** (dedicated task pinned in `ConsoleDisplay::begin()`): LVGL
  rendering and touch polling. UI updates from Core 0 grab `lv_lock()` before
  touching any `lv_*` API (LVGL 9 built with `LV_USE_OS = LV_OS_FREERTOS`).

### Module overview

| Module | File | Responsibility |
|--------|------|----------------|
| Display / LVGL | `src/display.cpp` | LovyanGFX init, LVGL registration, Core-1 task |
| On-device UI | `src/ui/ui.{h,cpp}` | Splash / onboarding / home / OTA screens |
| WiFi provisioning | `src/wifi_provisioning.cpp` | AP + captive portal, STA reconnect, mDNS |
| Web server | `src/web_server.cpp` | `/api/*` + `/ws` + static SPA (SPIFFS) |
| OTA | `src/ota.cpp` | HTTP(S) firmware update with progress callback |
| Protocol | `src/protocol.cpp` | Scale ↔ Console JSON tagged enums |
| Scale link | `src/scale_link.cpp` | SSDP discovery + WebSocket client to scale |
| SSDP listener | `src/ssdp_listener.cpp` | UDP multicast NOTIFY parsing |
| NFC reader | `src/nfc_reader.cpp` | PN532 poll + NDEF URI decode |
| Spool store | `src/store.cpp` | JSONL on LittleFS with in-RAM indexes |
| Spool record | `src/spool_record.cpp` | Struct + JSON (de)serialization |

### Partitions (`partitions.csv`)

| Name | Type | Size | Role |
|------|------|------|------|
| nvs | data/nvs | 20 KB | Configuration (WiFi, OTA, scale, printers) |
| otadata | data/ota | 8 KB | Active-partition marker |
| app0 / app1 | app/ota | 3 MB each | Dual OTA firmware slots |
| spiffs | data/spiffs | 3 MB | Frontend bundle (overwritten by `uploadfs`) |
| userfs | data/spiffs (LittleFS-mounted) | 7 MB | Spool DB + user data (survives FW/FS updates) |

### NVS

| Namespace | Key | Description |
|-----------|-----|-------------|
| `wifi_cfg` | `ssid`, `pass`, `device_name`, `fixed_key` | WiFi + device identity |
| `ota_cfg`  | `url`, `use_ssl`, `verify_ssl` | OTA server settings |
| `scale_cfg` | `ip`, `name`, `secret` | Paired scale (populated by SSDP) |
| `printers_cfg` | `list` | Printer list JSON (M2) |

### Scale ↔ Console protocol

The console is the **client**; it connects to `ws://<scale>:81/ws` after SSDP
discovery finds a device with URN `urn:spoolhard-io:device:spoolscale`.

Wire format mirrors the Rust reference (`yanshay/SpoolEase:shared/src/scale.rs`),
serde externally-tagged enums over text frames. See
[scale/firmware/ARCHITECTURE.md](../../scale/firmware/ARCHITECTURE.md) for
the full message list — both directions are symmetric with the console.

### REST API (port 80)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET  | `/api/wifi-status` | Current WiFi state |
| GET/POST | `/api/device-name-config` | Device / mDNS name |
| GET/POST | `/api/ota-config` | OTA server settings |
| POST | `/api/ota-run` | Trigger firmware update |
| POST | `/api/restart` | Soft reboot |
| POST | `/api/reset-device` | Factory reset |
| GET  | `/api/test-key` | Security key metadata (masked preview) |
| POST | `/api/fixed-key-config` | Set the shared secret with the scale |
| GET  | `/api/firmware-info` | Versions + partition usage + heap |
| GET  | `/api/spools` | Paginated spool list (`?offset`, `?limit`, `?material`) |
| GET  | `/api/spools/{id}` | Single spool record |
| POST | `/api/spools` | Upsert |
| DELETE | `/api/spools/{id}` | Remove |
| GET  | `/api/scale-link` | Paired scale status |
| POST | `/api/scale-link/tare` | Forwards `Calibrate(0)` to the scale |
| POST | `/api/scale-link/calibrate` | Forwards `Calibrate(w)` |
| POST | `/api/scale-link/read-tag` | Forwards `ReadTag` |
| WS   | `/ws` | Debug broadcast: `scale_link`, `weight`, `scale_tag`, `nfc` |
