# SpoolHard

Open-source firmware and web interfaces for the SpoolHard filament management ecosystem. A fork of sorts of SpoolEase.

This could have not been possible without Claude Code - it wrote every single line of code as I focused on the design and
the workflow I want(ed) to have. 

The repo ships two products that talk to each other over the local network, share a React design system via a workspace package, and sit behind a single shared security key:

- **Scale** — an ESP32-S3 load cell with NFC, built from scratch.
- **Console** — an ESP32-S3 touch display (WT32-SC01-Plus) that drives Bambu Lab printers over MQTT, pairs with the scale over WebSocket, and manages a spool inventory locally on microSD or in on-flash LittleFS.

## Lineage

SpoolHard is a fork of [yanshay/SpoolEase](https://github.com/yanshay/SpoolEase) — the original Rust firmware + the protocol it speaks. The upstream project's docs at [docs.spoolease.io](https://docs.spoolease.io/) cover the hardware, the design rationale, and the build instructions, including:

- [Console build guide](https://docs.spoolease.io/docs/build-setup/console-build)
- [Scale build guide](https://docs.spoolease.io/docs/build-setup/scale-build)

Follow those for assembly — the BOM and physical setup are identical. This repo only diverges in the firmware/software layer.

> Honest origin story: I started wanting a simple visual redesign of the existing SpoolEase UIs. Then I got carried away — porting the firmware from Rust to Arduino/C++, swapping the LCD stack to LVGL 9 + LovyanGFX, rebuilding both frontends in React 19 + Tailwind v4… all the way down to a different OTA flow and a renamed brand. Past a certain point a "fork" became fairer than calling it a redesign, hence this repo. The upside: I want this firmware to be open and opensource - there are 8 different ways to build/flash your firmware in any way you want - it's yours to do what you want. 

## Repository layout

```
spoolhard/
  scale/
    firmware/          ESP32-S3 PlatformIO project (Arduino/C++17)
      include/
      src/
      platformio.ini
      partitions.csv
    frontend/          React 19 + Vite + TailwindCSS v4 SPA
      src/
    scripts/           build_frontend.py, generate_manifest.py, release.sh
    release/           ← output of release.sh (firmware.bin, spiffs.bin, manifest.json)

  console/
    firmware/          ESP32-S3 PlatformIO project with LVGL 9 + LovyanGFX
      include/
      src/
      platformio.ini
      partitions.csv
    frontend/          React 19 + Vite SPA (same design system as the scale)
      src/
    scripts/           build_frontend.py, generate_manifest.py, patch_lvgl.py, release.sh
    release/           ← output of release.sh

  shared/
    frontend/          @spoolhard/ui workspace package (Button, StatCard,
                       StatusDot, SubTabBar, LoginPage, AuthProvider, theme tokens, …)

  flasher/             Browser-based USB flasher (Vite + React + esp-web-tools)
                       — fetches the latest GitHub release and writes both
                       products straight to the connected board over Web Serial.

  docs/                Design system + architecture notes
```

## Products at a glance

### Scale
- **Hardware:** ESP32-S3-WROOM + HX711 load cell, PN532 NFC, WS2812 LED.
- **Firmware:** PlatformIO/Arduino; WebSocket JSON protocol on `:81/ws` for the console link; custom SSDP NOTIFY on UDP `239.255.255.250:1990`.
- **Frontend:** served from SPIFFS; exposes dashboard, calibration, WiFi/OTA/security config.

See [`scale/firmware/ARCHITECTURE.md`](scale/firmware/ARCHITECTURE.md).

### Console
- **Hardware:** WT32-SC01-Plus (ESP32-S3, 480×320 capacitive touch LCD, 8-bit parallel bus, FT5x06 touch), PN532 NFC on the extended JST header, onboard microSD.
- **Firmware:** PlatformIO/Arduino; LVGL 9 + LovyanGFX on Core 1, networking on Core 0; Bambu Lab MQTT+TLS (up to 5 printers), Bambu SSDP discovery on `:1990` and `:1900`, scale WebSocket client, NFC reader, on-device LVGL UI.
- **Frontend:** React SPA with the shared `@spoolhard/ui` components; Dashboard, Spools, Printers config, Scale discovery + pairing, OTA.
- **Storage:** dual filesystem — SPIFFS for the frontend bundle, a separate LittleFS `userfs` partition for the spool database so user data survives firmware/frontend updates.

See [`console/firmware/ARCHITECTURE.md`](console/firmware/ARCHITECTURE.md).

---

## Prerequisites

| Tool | Used for | Install |
|---|---|---|
| **Node.js ≥ 18** + **npm** | Frontend build (Vite, TypeScript, TailwindCSS v4) | `nvm install 22` or your OS packages |
| **PlatformIO Core** (`pio`) | Firmware build, flashing, monitor | `pipx install platformio` or the PlatformIO VS Code extension |
| **Python 3** | Release scripts + manifest generation | usually pre-installed |
| **gzip**, **sha256sum** | Release bundling | usually pre-installed on Linux/macOS |

The frontend workspaces are declared in the root `package.json`. Run `npm install` once at the repo root to hydrate all three frontend projects (shared, scale, console).

```bash
npm install          # at repo root — installs all three frontend workspaces
```

---

## Versioning

Each product carries its own version file. The firmware and the frontend always share that one version:

```
scale/VERSION       0.1.0.alpha-1
console/VERSION     0.1.0.alpha-1
```

The value is read at build time by a PlatformIO pre-script (`scripts/patch_version.py`) which injects it as `-DFW_VERSION=…` / `-DFE_VERSION=…` compile-time defines, so `include/config.h` never needs to be edited for a release. Bump the version by editing the file — nothing else.

Suggested scheme: `MAJOR.MINOR.PATCH.stage-N` (`0.1.0.alpha-1`, `0.1.0.beta-3`, `0.1.0`). Any string works; it shows up verbatim in the web UI header and in each `manifest.json`.

### Release flow

Before tagging a release, refresh the changelog from `git log` since the
previous tag:

```bash
./scripts/update_changelog.sh   # writes a new section into CHANGELOG.md
```

The script pulls commit subjects since the last `v*` tag and ignores
anything tagged `[chore]`. Review the new section, edit if needed, then:

```bash
git add CHANGELOG.md console/VERSION scale/VERSION
git commit -m "[chore] release v$(cat console/VERSION)"
git tag    "v$(cat console/VERSION)"
git push --follow-tags
```

See [`CHANGELOG.md`](CHANGELOG.md) for the release history.

## Quick build

### Both products in one go (recommended)

```bash
./scripts/release.sh \
  --scale-base-url   https://your-ota.example.com/bins/scale/0.1 \
  --console-base-url https://your-ota.example.com/bins/console/0.1
```

Output:

```
release/
  scale/
    firmware.bin
    spiffs.bin
    manifest.json
  console/
    firmware.bin
    spiffs.bin
    manifest.json
```

Options:

- `--scale-base-url` / `--console-base-url` — the URL prefix the device will prepend to the binary filenames when polling `manifest.json`. Omit either one to get a placeholder URL for local testing.
- `--output-dir <path>` — override the default `release/`.
- `--only scale` / `--only console` — build a single product.
- `--with-filaments` — also build `release/filaments.db` from the upstream Bambu profiles (see below). Off by default — pulls a sparse clone of `bambulab/BambuStudio` and needs `uv` installed.
- `--filaments-ref <branch>` — pin a specific BambuStudio branch/tag (default `master`); implies `--with-filaments`.

### Browser flasher (Web Serial)

[`flasher/`](flasher/) is a small React app that uses [esp-web-tools](https://esphome.github.io/esp-web-tools/) to flash a connected SpoolHard board straight from a Chrome / Edge browser tab — no PlatformIO, no esptool install. It fetches the latest GitHub Release via the public API and feeds esp-web-tools the per-product `flasher-manifest.json` from that release.

**Local development**:

```bash
cd flasher
npm run dev          # → http://localhost:5173
```

**Hosting**: a GitHub Actions workflow at [`.github/workflows/deploy-flasher.yml`](.github/workflows/deploy-flasher.yml) builds and publishes `flasher/dist/` to GitHub Pages on every push to `main` that touches the flasher (and on every `v*` tag). Configure once in repo Settings → Pages → Source = "GitHub Actions".

**Custom domain (CNAME)**:

1. Set a repo variable (Settings → Secrets and variables → Actions → Variables) called `PAGES_CUSTOM_DOMAIN` to e.g. `flasher.spoolhard.io`. The workflow plants this as a `CNAME` file inside the deployed artifact on each run.
2. DNS — add a `CNAME` record from `flasher.spoolhard.io` → `fabbarix.github.io`. (For an apex like `spoolhard.io`, use four `A` records pointing at GitHub's anycast IPs `185.199.108.153`, `185.199.109.153`, `185.199.110.153`, `185.199.111.153` instead.)
3. In Settings → Pages, fill in the custom domain field and tick "Enforce HTTPS" once the cert provisions (~minutes).

**No CORS pain**: `api.github.com` and `objects.githubusercontent.com` (where release assets are served) both send `Access-Control-Allow-Origin: *`, so the page can fetch metadata + `.bin` payloads directly with no server-side proxy.

**Release-side**: each product's `release.sh` now also bundles `bootloader.bin`, `partitions.bin`, `boot_app0.bin` and writes a `flasher-manifest.json` alongside the firmware/frontend bins, so a clean fresh-flash from the browser works against any tagged release.

### Filaments database

The console reads a `filaments.db` SQLite file (uploaded via the web UI) for material defaults, color presets, and temperature ranges. The DB is generated from the Bambu Studio open-source filament profiles at [`bambulab/BambuStudio:resources/profiles/BBL/filament`](https://github.com/bambulab/BambuStudio/tree/master/resources/profiles/BBL/filament).

```bash
./scripts/build_filaments_db.sh                       # → release/filaments.db
./scripts/build_filaments_db.sh path/to/out           # → custom output dir
./scripts/build_filaments_db.sh --ref some-branch     # pin a non-master ref
```

The script sparse-clones just the filament-profiles directory (avoids fetching the full BambuStudio repo), runs the parser at [`scripts/bambu-filaments/`](scripts/bambu-filaments/), and writes a deduplicated `filaments.db`. ~150 KB output, distributable as a separate release artifact.

### One product only

Each product also carries its own `scripts/release.sh` with the same build steps — drop into the product directory and run it if you want to iterate on just one:

```bash
cd scale
./scripts/release.sh --base-url https://your-ota.example.com/bins/scale/0.1
# → scale/release/{firmware.bin, spiffs.bin, manifest.json}

cd ../console
./scripts/release.sh --base-url https://your-ota.example.com/bins/console/0.1
# → console/release/{firmware.bin, spiffs.bin, manifest.json}
```

The top-level `scripts/release.sh` is just a thin wrapper over these two.

The `manifest.json` carries the version, binary sizes, and SHA-256 digests so an OTA server can validate downloads.

---

## Build artifacts (what you upload)

Each product produces the same three files in its `release/` directory:

| File | Typical size | What it is | Where it goes |
|---|---|---|---|
| `firmware.bin` | ~1 MB (scale) / ~2 MB (console) | ESP32 application image — the compiled firmware | App partition (`app0` / `app1`) via `POST /api/upload/firmware` or `pio run -t upload` |
| `spiffs.bin` | ~2 MB (scale) / ~3 MB (console) | SPIFFS filesystem containing the gzipped React bundle | SPIFFS partition via `POST /api/upload/spiffs` or `pio run -t uploadfs` |
| `manifest.json` | ~500 B | Version + size + SHA-256 for both of the above | Served alongside the two `.bin` files for OTA clients |

Firmware and frontend are independent — you can ship a new frontend without touching the firmware, and vice versa. Uploads reboot the device on success. Both upload endpoints require the device's security key when one is set (see [Security](#security)).

---

## Building piece-by-piece

### Frontend only

```bash
cd scale/frontend   # or console/frontend
npm run build       # → ../firmware/data/ (gzipped SPA)
```

The `build_frontend.py` PlatformIO pre-hook runs this automatically when you invoke `pio run -t buildfs`, so you don't normally need to run it by hand.

### Firmware only

```bash
cd scale/firmware   # or console/firmware
pio run             # compiles, produces .pio/build/esp32-s3/firmware.bin
```

### SPIFFS image only

```bash
cd scale/firmware   # or console/firmware
pio run -t buildfs  # → .pio/build/esp32-s3/spiffs.bin
```

---

## Flashing

### Over USB (first-time flash or recovery)

```bash
cd scale/firmware   # or console/firmware
pio run -t upload    --upload-port /dev/ttyACM0    # firmware
pio run -t uploadfs  --upload-port /dev/ttyACM0    # frontend (SPIFFS)
```

Serial port varies by OS (`/dev/ttyACM0`, `/dev/ttyUSB0`, `COM3`, …). The console is also flashable over the ESP32-S3 native USB JTAG — no external programmer needed.

### Over the air (from the web UI)

1. Open the device's web UI in a browser.
2. Config → Device → Firmware upload → drag `firmware.bin` onto the drop zone.
3. Config → Device → Frontend upload → drag `spiffs.bin`.

The device rebooots on success. Uploads are authenticated — you'll have signed in with the security key before the upload forms show up.

### Over the air (from a Web flasher / CI)

The `manifest.json` in each product's `release/` directory is intended to be served at a stable URL. The device polls it on boot or on demand, compares versions with its own, and pulls `firmware.bin` / `spiffs.bin` if they differ. The upload flow for an OTA server is:

```
your-ota-server.example.com/
  scale/ota/
    firmware.bin
    spiffs.bin
    manifest.json
  console/ota/
    firmware.bin
    spiffs.bin
    manifest.json
```

---

## Development

### Hot-reload the frontend against a real device

```bash
cd scale/frontend   # or console/frontend
VITE_DEVICE_IP=192.168.1.50 npm run dev
```

Vite proxies `/api`, `/captive`, and `/ws` through to the device so the frontend loads from your workstation while API + WebSocket traffic still hits the real ESP32.

### Monitor serial

Interactive session:

```bash
cd scale/firmware   # or console/firmware
pio device monitor -b 115200
```

Quit with **Ctrl-C** (or `Ctrl-T Q` if you're inside `pio device monitor`'s terminal).

#### Capture a short window (scripted, for bug reports and agents)

`pio device monitor` is interactive, which doesn't compose well with scripted
tools or AI agents that just want to dump the device's output around a
specific event (scan a tag, press a button, trigger a crash). A tiny Python
one-liner does the job without extra deps — most dev boxes already have
`pyserial` via PlatformIO. Pattern:

```bash
python3 << 'EOF'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
print("Capturing for 15 seconds — do your thing now...")
end = time.time() + 15
buf = b''
while time.time() < end:
    c = s.read(4096)
    if c: buf += c
s.close()
print(buf.decode('utf-8', errors='replace') or '(nothing received)')
EOF
```

Replace `/dev/ttyACM0` with whatever your board enumerates as (`ls /dev/ttyACM*` / `ls /dev/ttyUSB*`). The ESP32-S3 usually shows up as `/dev/ttyACM0` because it exposes a native USB-CDC interface.

#### Reset + capture boot log

Handy when you want a clean boot log, or when the device is silent because it's waiting on something. Pulse RTS low to reset the ESP32 (EN pin), then read:

```bash
python3 << 'EOF'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + 10
buf = b''
while time.time() < end:
    c = s.read(4096)
    if c: buf += c
s.close()
print(buf.decode('utf-8', errors='replace'))
EOF
```

#### Decode a panic backtrace

When the device dumps a `Guru Meditation Error` with a backtrace of raw hex
addresses, run them through `addr2line` against the ELF to get
file:line positions. The toolchain ships with PlatformIO:

```bash
cd console/firmware   # (or scale/firmware)
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/esp32-s3/firmware.elf \
  0x42038d21 0x42038de1 0x420310d5   # …paste the backtrace addresses
```

`-C` demangles C++ names, `-i` expands inlined frames, `-p` prints a readable form.

---

## Security

Both products share a single **security key** stored in NVS (`wifi_cfg/fixed_key`). When it's left at the default (`"Change-Me!"`) or empty, auth is disabled — useful during provisioning.

When it's set to anything else, every `/api/*` request must carry:

```
Authorization: Bearer <key>
```

Missing or wrong → `401 Unauthorized`. Firmware / SPIFFS uploads check the key on the first chunk and call `Update.abort()` on rejection, so an unauthorized upload can't brick the device.

In the browser, `AuthProvider` shows a **Locked** screen (with a "Remember this device" checkbox that picks between `localStorage` and `sessionStorage`). Once signed in, a patched `fetch` appends the header to every `/api/*` request automatically — hooks don't need to know about auth. Keys are scoped per origin, so the scale and console (e.g. `spoolhard-scale.local` and `spoolhard-console.local`) remember separately.

Set the key either from the web UI (Config → Security) or directly via:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"key":"your-secret"}' \
  http://<device>/api/fixed-key-config
```

A factory reset via the onboard button sequence (or `POST /api/reset-device` with the current key) clears WiFi + the security key + device name.

---

## Design system

`shared/frontend/` is a workspace package (`@spoolhard/ui`) consumed by both the scale and console frontends. It owns the TailwindCSS v4 theme, typography, animations, and the common UI primitives: `Button`, `Card`, `SectionCard`, `StatCard`, `StatusDot`, `InputField`, `PasswordField`, `DropZone`, `SubTabBar`, `LoginPage`, `TextScanner`, plus `QueryProvider`, `AuthProvider`, and `WebSocketProvider`.

See [`docs/UI_UX_DESIGN_SYSTEM.md`](docs/UI_UX_DESIGN_SYSTEM.md) for the complete visual language.

---

## License

See [LICENSE](LICENSE).
