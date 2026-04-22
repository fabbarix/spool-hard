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
