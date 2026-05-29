# Changelog

All notable changes to SpoolHard are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

New entries are appended automatically by `scripts/update_changelog.sh`,
which pulls commit subjects from `git log <previous-tag>..HEAD` and drops
anything tagged `[chore]`. See the script header for the full release flow.

## [0.12.6] - 2026-05-29

Console: on-device config screen on the LCD.

- feat(lcd): new Device screen reached by tapping the home
  footer strip (gear glyph hints tappability). Two tabs
  toggled by a header segment control:
  - **Settings**: display sleep-timeout presets (Never / 30s /
    1m / 2m / 5m / 15m, with a "Current: N s" readout for a
    web-set custom value); "Check for updates" (kicks both the
    console and the paired scale); a push-driven status line
    (Checking… / Update available / Up to date / Check
    failed); conditional "Apply update" (console) and "Apply
    scale update" buttons that appear only when that device
    has a pending update; and "Restart". Apply + Restart go
    through a confirm dialog.
  - **Info**: read-only host / IP / firmware / frontend / free
    heap / scale link + version.
- feat(lcd): reusable `ui_show_confirm()` full-screen confirm
  (captures + restores the prior screen on cancel).
- feat(lcd): applying a scale update from the console mirrors
  the scale's self-flash progress onto the shared OTA screen
  and routes back to the Info tab once the scale reconnects on
  the new version.
- refactor(display): `setAndPersistSleepTimeout()` shared by
  the web `/api/display-config` handler and the LCD so the
  clamp + NVS keys can't drift; a new `state.display_config`
  WS broadcast keeps an open web client in sync when the value
  is changed on the device (and vice versa).
- fix(dashboard): suppress the "Link to filament" affordance
  while the user-filaments + stock-filaments queries are still
  in flight. On dashboard reload the printer state is push-
  cached and renders immediately, but the library queries fire
  a tick later — populated slots briefly read as "unmatched"
  before the libraries arrive, causing the link prompt to
  flash. Now gated on a `librariesReady` flag.

## [0.12.3] - 2026-05-26

Console: link spools and AMS slots to Bambu's slicer presets
end-to-end, plus push user filaments back to Bambu Cloud.

- fix(bambu): normalize Bambu's MQTT tag_uid suffix. Bambu
  publishes 4-byte tag UIDs as 16 hex chars padded with the
  constant tail `00000100`; the console's PN532 stores the
  raw 4-byte UID (8 hex chars), so `SpoolStore::findByTagId`
  never matched and AMS slot weights silently rendered as
  "—". The MQTT ingest paths in `bambu_printer.cpp` now strip
  the documented suffix on the way in. 7-byte (NTAG2xx) UIDs
  are unchanged.
- feat(dashboard): "Link to filament" affordance on AMS slot
  cards that report a `tray_info_idx` no filament-DB row
  claims yet — the most common case for custom Studio presets
  that came in from Bambu Cloud sync without a `filament_id`.
  Click → modal lists user-filament rows filtered by material;
  pick one and the chosen row's `filament_id` is set to the
  slot's preset code. Stock Bambu codes (`GF…`) are
  recognised against the bundled filament library and
  suppress the affordance. Future spools linked to that
  filament then ship the right `tray_info_idx` on their
  first AMS load.
- feat(bambu): `_publishAmsFilamentSetting` falls back to the
  linked filament's `filament_id` when the spool's own
  `slicer_filament` is empty. Closes the "wrong-settings
  window" on a freshly-paired spool that's never been in an
  AMS slot before — the moment the user picks the filament
  preset in the wizard, the very first MQTT push ships the
  right `tray_info_idx` without waiting for an auto-import
  round-trip.
- fix(bambu-cloud): rewrite the user-filament cloud-push
  payload to match Bambu's actual stored format. Per-
  extruder numeric fields (`nozzle_temperature`,
  `nozzle_temperature_initial_layer`,
  `nozzle_temperature_range_low/high`,
  `filament_max_volumetric_speed`, `pressure_advance`) ship
  as N-element comma-joined strings; `filament_extruder_variant`
  ships as semicolon-joined with each segment escape-quoted;
  identity strings (`filament_vendor`, `filament_type`,
  `filament_settings_id`, `compatible_printers`) ship as
  single escape-quoted strings. Also adds `inherits`,
  `from:"User"`, in-setting `version:"2.6.0.2"`, and
  `compatible_printers` (parsed from `cloud_inherits` when
  the variant didn't capture printer_model + nozzle_diameter
  during sync). Skips `enable_pressure_advance:"1"` when all
  stored PA values are zero. Previously the cloud rejected
  every push with HTTP 422 "Invalid input parameters"; now
  multi-extruder presets like "Tinmorry PETG-CF" push
  cleanly.

## [0.12.0] - 2026-05-23

Scale: surface "uncalibrated" as its own LED state and expose
the full LED palette to the operator.

- feat(led): new `showUncalibrated()` state — magenta 1 Hz
  flash, held continuously while the load cell has no
  calibration points stored. Takes priority over the network
  ladder so a paired-but-uncalibrated scale reads as "needs
  config" rather than "looks fine". Sits below NFC and weight
  overlays so live calibration captures still flash their
  burst through.
- feat(api): `GET /api/led-legend` returns a JSON catalog of
  every LED state (id, label, kind, color, period, description).
  `POST /api/led-test?state=<id>&ms=<duration>` pins the
  requested pattern over normal arbitration for up to 30 s
  via a single-writer atomic-flag handoff to `loopTask`.
- feat(config UI): new LED Legend section under Setup. Renders
  the catalog with CSS animations that match the firmware
  patterns (flash/pulse/burst) and a per-row "Test" button
  that fires the pattern on the physical LED for 5 s.

## [0.11.7] - 2026-05-23

Console: Bambu printer SSDP discovery — listen on the channel
printers actually use, surface discovered ones in the UI.

- fix(discovery): add a UDP/2021 broadcast listener. Earlier
  code only listened on multicast 239.255.255.250:1990/1900;
  observed traffic and `inindev/bambu-bridge` both show
  X1/P1/H2D/O1S broadcasting their NOTIFY frame to
  255.255.255.255:2021 instead, so configured printers reached
  on a fresh DHCP lease couldn't be re-discovered. The existing
  on-announce → BambuManager wiring now picks up IP changes
  via this new channel.
- fix(ssdp): `SsdpListener::begin` gained a `joinMulticast`
  flag so the broadcast path uses `AsyncUDP::listen()` instead
  of `listenMulticast()` and `sendMSearch` targets
  255.255.255.255 in that mode.
- feat(discovery): parse `DevName.bambu.com` and
  `DevModel.bambu.com` from the NOTIFY frame. The user-set
  printer name (e.g. "Bambozzo") and model code (e.g. "O1S",
  "X1C") are now exposed on `/api/discovery/printers`.
- feat(config UI): the Printers tab now shows a dedicated
  "Discovered on network" list of unconfigured printers above
  the Add-Printer button. Each row has its own Add button that
  pre-fills the form with the printer's name, serial, and IP,
  leaving only the access code to enter. Replaces the older
  flow that hid the discovered list inside the Add panel.

## [0.11.0] - 2026-05-09

Catch-up release rolling up everything since v0.7.0. Headline fix:
the scale↔console WebSocket link no longer cycles every 17–19 s.

- fix(ws): upgrade both products from `mathieucarbou/ESPAsyncWebServer`
  3.0.6 + `AsyncTCP-esphome` 2.1.4 to `esp32async/ESPAsyncWebServer`
  3.11.0 + `esp32async/AsyncTCP` 3.4.10. The old pairing had a
  send-queue drain stall (lib's `_messageQueue` accumulated frames
  that never reached TCP — `tcp_sndbuf` stayed at full-of-free-space
  the whole cycle); once the queue hit `WS_MAX_QUEUED_MESSAGES=32`,
  `closeWhenFull=true` slammed the connection and the console
  reconnected. ESP32Async v3.10.0 refactored the WS send/queue path
  (PR #383/#387/#388) and v3.11.0 flipped `closeWhenFull` default to
  `false` (PR #434), eliminating both halves of the failure mode.
  Validated on hardware: 7+ minutes of continuous link, qlen never
  above 1, `sndbuf` actively cycles.
- feat(scale): WebSocket queue/state diagnostics surface at
  `/api/logs` — `ws-stats` per-second tick (qlen / peak / cansend /
  sndbuf / status / txA / txF / rxF / age), `ws-tx` snapshot when
  outbound queue gets unhealthy, `DISCONN` summary on every
  WS_EVT_DISCONNECT.
- chore(scale): add `-DASYNCWEBSERVER_REGEX=1` to scale's build_flags
  (console always had it). The new lib hard-errors at compile time
  when `pathArg()` is used without it; the old lib silently treated
  the regex as a literal string, which is why the
  `/api/crashes/<seq>` route fell through to the listing endpoint.
- feat(scale): 0.8.0+ task-model split — sensor_task (HX711, 100 Hz),
  nfc_task (PN532 SPI, 50 Hz), console_tx_task (single writer to
  `/ws/console`), AsyncTCP_task (mathieucarbou's lib + lwIP + WiFi);
  the coordinator `loopTask` no longer blocks on hardware I/O.
  Single-port web server (port 80) with `/ws` (browser dashboard)
  and `/ws/console` (paired console).
- feat(infra): new shared firmware library `spoolhard_core` carrying
  OTA, product-signature + version markers, NVS-namespace backup /
  restore, ring log + `dlog`, panic-persist, PSRAM JSON allocator,
  WS buffer pool, auth, and common routes. Both products consume
  it as a PlatformIO local library; identity comes in via build_flags
  (`-DPRODUCT_ID`, `-DPRODUCT_NAME`, `-DOTA_DEFAULT_URL`).
- feat(console+scale): panic-persist crash logger — boot-time
  promotion of pending ring-log tail to `/crash_<seq>.txt` when the
  previous reset reason was crashy; surfaced via `/api/crashes` +
  `/api/crashes/<seq>` for retrieval over HTTP.
- feat(console): H2S (Bambu model O1S) printer support — bigger
  MQTT packet buffer (32 KB; H2S pushall is ~17 KB), no SSDP fallback,
  FTPS data-port LIST-then-handshake ordering quirk, self-healing
  per-printer NVS cache + manual reset endpoint.
- fix(console): Bambu AMS sync — uppercase RGBA before publishing
  `ams_filament_setting`. Bambu's printer silently drops mixed/lower-
  case `tray_color` and reverts to black.
- fix(console): Bambu FTPS — raw-mbedtls FTPS with TLS session reuse,
  PSRAM allocator override, analyzer reset() restoration.
- feat(console): print-analysis improvements — G2/G3 arc parsing,
  `; filament_ids/colour/density` slicer-header parsing, `.bbl`
  companion JSON for AMS mapping, 4-tier slicer-tool → physical-AMS-
  slot resolver, streaming inflate for ZIP method=8.
- feat(infra): release artifacts now include bootloader / partition
  table / boot_app0 alongside firmware / frontend, plus an
  `flasher-manifest.json` for esp-web-tools browser flashing.

## [0.5.9] - 2026-04-27

Print-analysis accuracy + AMS-slot resolution overhaul, plus
heap-pressure resilience for the FTPS analyser.

- feat(analyzer): parse G2/G3 arc moves. Bambu's slicer rewrites runs
  of short G1 segments as arcs by default, so the old "G1 only" path
  was silently under-counting extruded length by ~30 % on any print
  with curved walls (1.72 m → 1.2 m kind of miss).
- feat(analyzer): parse `; filament_ids`, `; filament_colour`, and
  `; filament_density` slicer-header comments into a per-slot table.
  Densities also override the family-default so gram totals stay
  accurate on mixed-material prints.
- feat(analyzer): fetch the `.bbl` companion JSON
  (`/cache/<N>_<name>.gcode.bbl`) right after the gcode and parse its
  `ams mapping` array.
- feat(analyzer): 4-tier slicer-tool → physical-AMS-slot resolver —
  `.bbl ams_mapping[i]` first, then `filament_ids[i]` matched against
  each tray's `tray_info_idx` (incl. `vt_tray`), then `filament_colour`
  RGB-prefix matched against `tray_color`, then the live
  `active_tray` for single-tool prints. Fixes the case where a
  single-tool slicer T4 was reporting `tool_idx=4 / ams_unit=-1 /
  spool_id=""` instead of the loaded AMS slot.
- feat(analyzer): streaming inflate for ZIP method=8 entries via the
  ROM-bundled miniz `tinfl_decompress`, with a 32 KiB PSRAM sliding
  dictionary. Pipes FTP retrieve-range chunks through the inflater so
  the analyser can read deflated 3MFs without buffering the whole
  archive in RAM.
- feat(infra): `BambuPrinter::startAnalyseTask` (and equivalents for
  the FTP-debug and MQTT-connect tasks) now allocates the FreeRTOS
  stack in internal DRAM first, falling back to a static PSRAM-backed
  stack when contiguous internal heap is fragmented (typical mid-print
  with MQTT pushall + mbedtls + LVGL all live).
- fix(ui): LCD "Reconnect" button was silently failing under heap
  pressure because the MQTT-connect task spawn returned `pdFAIL`. The
  PSRAM fallback above plus a `dlog("ui", "reconnect tapped …")` in
  the handler make the button work and the failure visible in
  `/api/logs?tag=ui`.
- fix(web): `POST /api/printers/<serial>/analyze` now rejects with
  409 when the printer isn't `RUNNING` or `PAUSE`. Bambu tears the
  FTPS cache down within seconds of `FINISH` and the task would
  otherwise race the teardown for an opaque mbedtls error.

## [0.5.4] - 2026-04-26

Tare + multi-point calibrate directly from the console LCD, plus a
batch of UX polish on top of v0.5.0.

- feat(scale): tap the home-screen Scale card to open a Scale settings
  view with live weight, calibration status, and Tare / Add point /
  Clear / Close actions.
- feat(scale): 3-step calibration wizard — pick known weights from a
  configurable preset list, place + capture (button gates on the
  scale's stable / new state, and on uncalibrated for the first
  bootstrap point after a Clear), confirm and add another or finish.
- feat(scale): chip taps in the wizard ADD into a running sum so the
  user can compose any reference weight from what they own
  (e.g. 100 g + 200 g = 300 g).
- feat(scale): prominent "Target: N g" label on the capture screen
  plus a signed delta-vs-target line that turns teal within ±1 % (or
  ±1 g) of the target, so the user can see at a glance when the load
  is close.
- feat(scale): web Config → Scale gains a "Calibration Weights"
  section to manage the LCD wizard's preset chips (sorted, deduped,
  capped at 12). Defaults to 100 / 250 / 500 g + 1 / 2 / 5 kg.
- feat(protocol): three new console↔scale messages —
  ConsoleToScale::AddCalPoint(weight), ConsoleToScale::ClearCalPoints,
  and ScaleToConsole::CalibrationStatus(num_points, tare_raw). Pushed
  on every tare / add / clear and once on console-connect so the LCD
  status line never has to poll. Console-side ScaleLink exposes the
  matching addCalPoint / clearCalPoints / onCalibrationStatus +
  cached calNumPoints / calTareRaw snapshot.
- fix(ui): AMS tile weight on both LCD and the web Printers panel now
  shows max(0, weight_current - consumed_since_weight) so the
  displayed remaining ticks down through a print as the gcode
  analyzer forecasts consumption — the breakdown is still on the
  detail screens for transparency.
- fix(scale fw): added `#include <HTTPClient.h>` to scale main.cpp so
  PlatformIO's LDF pulls in the bundled lib for spoolhard_core's
  ota.cpp. The console resolved it transitively via bambu_cloud.cpp;
  the scale had no equivalent caller and was failing to build locally.

## [0.3.1] - 2026-04-23

- fix(filaments): expanded row + edit form fall back to base_id when no inherits
- feat(filaments): edit form auto-fills from the cloud parent's settings
- feat(filaments): SD-cached public-catalog index + Config UI to manage it
- feat(filaments): "Try fetching parent from cloud" — public-catalog walker
- feat(filaments): per-preset cloud-detail viewer for cloud-synced customs
- feat(filaments): custom filaments inherit unset fields from a stock parent
- feat(backup): download/upload full-device backup for console + scale
- fix: OTA page no longer flashes "scale waiting / unavailable" on every WS reconnect

## [0.3.0] - 2026-04-22

- feat: bambu-filaments build pipeline emits filaments.jsonl (replaces SQLite); cloud-helper tools
- feat: Filaments tab + AMS tray_info_idx sync + per-spool empty marker + per-(filament, nozzle) PA + remote /api/logs

## [0.2.7] - 2026-04-22

Bambu Cloud login: paste-blob workflow + soft-verify.

- New helper script `tools/bambu_login.py` runs the Bambu Lab cloud
  login on a desktop (where Cloudflare lets the request through, since
  the WAF that blocks the ESP32 is keying off the device's TLS
  fingerprint). On success it prints a `SPOOLHARD-TOKEN:<base64>` blob
  carrying token + region + account label on a single line.
- The console firmware detects the prefix in the manual-paste field
  and unpacks the blob automatically; raw JWTs from other tools still
  work (no prefix, no behaviour change).
- `BambuCloudAuth::verifyToken` now returns a tri-state
  (`Verified` / `Rejected` / `Unreachable`). When the verify endpoint
  is itself blocked by Cloudflare, the token is saved anyway with an
  amber "couldn't verify" warning in the UI — a perfectly valid token
  no longer gets rejected just because the verify call hits the same
  WAF as the login.
- `looksLikeCloudflareBlock()` parses the static "Sorry, you have been
  blocked" template and extracts the Ray ID for diagnostics.
- TLS-fingerprint changes + framework upgrade (pioarduino /
  arduino-esp32 v3 / mbedtls 3.x for TLS 1.3) were investigated and
  deferred — even if it bypassed Cloudflare today it'd be a treadmill
  against bot-management updates, and the paste-blob flow handles the
  WAF case cleanly.

## [0.2.6] - 2026-04-22

OTA UX polish + Bambu Cloud login fix.

- Console LCD home screen now carries a non-modal "update available"
  banner across the bottom (Phase 3). The text reflects whichever
  products have updates pending — console, scale, or both — and a tap
  triggers the matching OTA(s) without leaving the home screen.
- "Update Now" buttons in the web UI now show progress feedback the
  whole way through: "Starting…" → "Updating firmware/frontend: X%" →
  "Rebooting…" → "✓ Updated to vX.Y.Z". Buttons are disabled while an
  update is in flight, status polling speeds up to 2 s for the
  duration, and React Query's auto-retry is off so connection failures
  during the reboot surface immediately as the rebooting cue.
- Server-side `OtaInFlight` tracker + extended `OtaProgressUpdate`
  wire frame (now carries `kind` + `percent`) so the console can
  surface the scale's flashing progress in the same banner.
- Bambu Cloud login redesigned to match the working
  `temp/bambu_login.py` reference — browser-shaped User-Agent +
  Origin/Referer headers, dropped the "agent" fingerprint headers
  that were tripping Cloudflare's WAF, and routed TFA to the website
  host (`bambulab.com/api/sign-in/tfa`) rather than the user-service
  host. Direct password login should work again.

## [0.2.5] - 2026-04-22

OTA overhaul + shared firmware library.

- New `shared/firmware/spoolhard_core/` PlatformIO library carries the
  manifest-driven OTA module (scheduled checker, sha256-verified
  firmware+frontend update, semver compare), the product-signature
  matcher, and the firmware-version marker parser. Both products link
  it via `lib_extra_dirs`; per-product identity (PRODUCT_ID, PRODUCT_NAME,
  OTA_DEFAULT_URL) now travels in `platformio.ini` build_flags so the
  library has no dependency on a per-product `config.h`.
- Scale gets full OTA parity with the console: scheduled manifest
  checker, NVS-backed `check_enabled` / `check_interval_h`, last-check
  telemetry, "Check now" + "Update now" buttons, single-product status
  panel.
- Scale ↔ console wire protocol gains three messages: `OtaPending`
  (scale pushes its pending state on connect / on change),
  `RunOtaUpdate` and `CheckOtaUpdates` (console-driven triggers). The
  console caches the scale's state and folds it into `/api/ota-status`
  so the React UI shows console + scale in one combined banner with
  per-product "Update now" buttons.
- OTA settings UI: toggles + interval picker collapsed onto a single
  row to save vertical space; auto-saves on every change (no Save
  button) — same behaviour on console and scale.
- New Bambu Cloud authentication section on the console (in progress —
  Cloudflare WAF blocks the ESP32's mbedTLS fingerprint on direct
  login, so the shipped path is manual token paste; full step-by-step
  diagnostics surface the raw response when a step fails).

## [0.2.3] - 2026-04-22

CI build fix. v0.2.2's release workflow failed at the console build step
because a fresh `^1.2.0` resolve picked LovyanGFX 1.3.x, whose vendored
LVGL adapter (`lgfx/v1/lv_font/font_fmt_txt.h`) collides with the real
lvgl package (duplicate enums / typedefs). Pinned LovyanGFX to 1.2.19
exactly, and extended `console/scripts/patch_lvgl.py` to also stub the
vendored lv_font headers if a future bump re-introduces them.

## [0.2.2] - 2026-04-22

Release pipeline only — no firmware-side changes from v0.2.1. Tag-triggered
GitHub Actions workflow now builds both products + the filaments DB and
publishes a Release with all assets renamed to the
`spoolhard-<product>-<file>` convention the browser flasher expects. The
v0.2.1 manifests referenced unprefixed names so the flasher couldn't find
them on the Release; v0.2.2 fixes that.

## [0.2.1] - 2026-04-21

First public release.
