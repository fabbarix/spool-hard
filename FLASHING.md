# Flashing SpoolHard firmware

Two devices, each carrying two artifacts:

- **firmware** — the ESP32 application image (`firmware.bin`).
- **frontend** — a React config UI packed into a SPIFFS image (`spiffs.bin`)
  that the device serves on port 80.

Both artifacts live on separate partitions and are flashed independently.
A firmware-only flash does not disturb the frontend and vice-versa.

## Build

Firmware:

```bash
cd console/firmware && pio run
cd scale/firmware   && pio run
```

→ `.pio/build/esp32-s3/firmware.bin`

Frontend (triggers the React build + gzip + product-signature step):

```bash
cd console/firmware && pio run -t buildfs
cd scale/firmware   && pio run -t buildfs
```

→ `.pio/build/esp32-s3/spiffs.bin`

## Console — USB/serial

The console (default mDNS: spoolhard-console.local) is an ESP32-S3 devkit that exposes a USB JTAG/serial
debug port. When plugged in it enumerates as `/dev/ttyACM0`.

Firmware:

```bash
cd console/firmware
pio run -t upload --upload-port /dev/ttyACM0
```

Frontend (SPIFFS partition):

```bash
cd console/firmware
pio run -t uploadfs --upload-port /dev/ttyACM0
```

Upload speed is already set to 921600 in `platformio.ini`. No password or key
is involved — physical access is the only gate.

To watch the serial log after flash:

```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

### Alternative — console OTA over the network

The console also runs a web server on port 80 and exposes the same two
multipart upload endpoints as the scale, gated by the same NVS fixed-key
scheme (different key per device — stored in your password manager under
`spoolhard-console`). mDNS name is **`spoolhard-console.local`**.

```bash
# Firmware
curl -X POST \
  -F "firmware=@console/firmware/.pio/build/esp32-s3/firmware.bin" \
  "http://spoolhard-console.local/api/upload/firmware?key=<KEY>"

# Frontend
curl -X POST \
  -F "frontend=@console/firmware/.pio/build/esp32-s3/spiffs.bin" \
  "http://spoolhard-console.local/api/upload/spiffs?key=<KEY>"
```

Use the USB path above when you have physical access — it's faster and
skips the key step.

## Scale — network (OTA)

The scale (default mDNS: spoolhard-scale.local) runs a web server on port 80 that exposes two
multipart/form-data upload endpoints — one for firmware, one for frontend.
Build the target `.bin` first, then POST it with `curl -F`.

### Find the scale

Discovery is via mDNS — the scale advertises itself under a user-chosen
hostname (set during onboarding). Current pairing is **`spoolhard-scale.local`**.

```bash
avahi-resolve -4 -n spoolhard-scale.local        # → 192.168.20.x
```

If mDNS is unavailable, the console's `/api/discovery/scales` endpoint lists
every paired scale with its IP, and the scale's own config page (port 80)
shows its IP in the header.

### Auth key

`POST /api/upload/firmware` requires a Bearer token or `?key=` query param
that matches the NVS-stored fixed key. Empty or default key ⇒ auth bypassed
(first-boot state). The key is per-device and set via the scale's web UI
("Device → Security → Fixed key") — store it in your local password manager,
do NOT commit it here.

For the scale, the key is tracked in your password manager under the
`spoolhard-scale` entry.

### Flash firmware

The endpoint uses **multipart/form-data** (`curl -F`), not raw body — the
scale's upload handler expects the AsyncWebServer file-part callback. Sending
a raw `--data-binary` body times out mid-transfer.

```bash
curl -X POST \
  -F "firmware=@scale/firmware/.pio/build/esp32-s3/firmware.bin" \
  "http://spoolhard-scale.local/api/upload/firmware?key=<KEY>"
```

A successful response is `{"ok":true}` and the scale reboots into the new
image. Watch its web UI for the new `FW_VERSION` string to confirm.

### Flash frontend (SPIFFS)

Same shape, different endpoint. Build the SPIFFS image first:

```bash
cd scale/firmware
pio run -t buildfs
curl -X POST \
  -F "frontend=@.pio/build/esp32-s3/spiffs.bin" \
  "http://spoolhard-scale.local/api/upload/spiffs?key=<KEY>"
```

The release script `scale/scripts/release.sh` bundles both artifacts plus a
manifest into `scale/release/` for distribution, but for local iteration the
two curl calls above are enough.

## Troubleshooting

- **"Not found" from the scale's HTTP root** — that's normal for arbitrary
  paths; the frontend lives at `/` and the API under `/api/…`. Use the
  device's actual web UI in a browser to verify liveness.
- **Console upload hangs** — unplug/replug the USB cable to drop whatever
  still holds `/dev/ttyACM0` (serial monitor, another `pio` run, etc.).
- **OTA succeeds but scale boots the old image** — the ESP32 OTA partition
  slots alternate; a corrupt image is rejected and the bootloader falls back.
  Check `pio device monitor` over the scale's config-port USB if you have
  physical access.
