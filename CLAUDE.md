# SpoolHard firmware — project notes for Claude

This repo holds two ESP32-S3 firmwares that talk to each other over a
WebSocket JSON protocol:

- `console/` — the wall-mounted console with LCD + PN532 NFC reader.
  Talks to Bambu printers over MQTT and to the scale over a port-81
  WebSocket client.
- `scale/` — the load-cell scale with its own PN532 NFC reader. Runs the
  WebSocket server the console connects to.

The two firmwares each have a PlatformIO project at `*/firmware/`, a
Vite/React config UI at `*/frontend/`, and a `VERSION` file.

## Flashing

See **[FLASHING.md](./FLASHING.md)** for the exact commands. TL;DR:

- Console: USB-attached on `/dev/ttyACM0` → `pio run -t upload` from
  `console/firmware/`.
- Scale: OTA via
  `POST http://<scale-hostname>.local/api/upload/firmware?key=<KEY>`
  with the `.bin` from `scale/firmware/.pio/build/esp32-s3/`. Default
  hostname after fresh provisioning is `spoolhard-scale.local`.

## Protocol

Wire format is a serde-externally-tagged JSON enum matching yanshay's
upstream `SpoolEase shared/src/scale.rs`. Both sides have a `protocol.h`
and `protocol.cpp` with mirrored enums:

- `ScaleToConsole::*` — events pushed from the scale (`NewLoad`,
  `LoadChangedStable`, `TagStatus`, `CurrentWeight`, …).
- `ConsoleToScale::*` — commands the console sends (`Calibrate`, `ReadTag`,
  `WriteTag`, `GetCurrentWeight`, `TagsInStore`, …).

When adding a new message type, edit **four** files: the enum in each
`protocol.h`, plus the `nameToType` / `typeToString` / `send` switches in
each `protocol.cpp`. Missing one side silently drops the frame.

## Building

Both firmwares build with plain `pio run` from their `firmware/` dir. The
release scripts under `*/scripts/release.sh` additionally build the
frontend, gzip it, produce a SPIFFS image, and emit a `manifest.json` for
distribution — only needed for actual releases.

## Shared firmware library: `spoolhard_core`

Code that's identical for both products lives in
**`shared/firmware/spoolhard_core/`** as a PlatformIO local library. Each
product's `platformio.ini` pulls it in via:

```ini
lib_extra_dirs = ../../shared/firmware
```

(no entry in `lib_deps` is required — PlatformIO LDF picks it up because
the headers are `#include`d).

**What's in the lib today**

- `spoolhard/ota.h` / `src/ota.cpp` — manifest-driven OTA: scheduled
  version checker, `OtaConfig` (NVS-backed), `otaRun()` runner that
  flashes firmware then frontend with sha256 verification.
- `spoolhard/product_signature.h` — `SPOOLHARD-PRODUCT=<id>` byte marker
  + `ProductSignatureMatcher` for streaming-upload validation.
- `spoolhard/version_marker.h` / `src/version_marker.cpp` —
  `SPOOLHARD-VERSION=<v>\x01` byte marker + `VersionMarkerParser` that
  pulls the version out of an uploaded firmware image.

**Per-product identity**: the library has no per-product config header
on its include path. Identity comes in as `-D` build flags from each
product's `platformio.ini`:

```
-DPRODUCT_ID=\"console\"            ; or "spoolscale"
-DPRODUCT_NAME=\"SpoolHard\ Console\"
-DOTA_DEFAULT_URL=\"https://…manifest.json\"
```

`FW_VERSION` / `FE_VERSION` are stamped in by
`scripts/patch_version.py` from each product's `VERSION` file at build
time. The library headers `#error` out if any of these macros are
missing.

**NVS schema (OTA)** is owned by `spoolhard_core/src/ota.cpp` — namespace
`ota_cfg`, keys `url`, `use_ssl`, `verify_ssl`, `ck_en`, `ck_hrs`,
`lck_ts`, `lck_st`, `lk_fw`, `lk_fe`. Both products use the same
schema; do not re-define those keys in the per-product `config.h`.

When adding new shared code, prefer extending the lib over duplicating
between `console/` and `scale/`.
