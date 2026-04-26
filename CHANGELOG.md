# Changelog

All notable changes to SpoolHard are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

New entries are appended automatically by `scripts/update_changelog.sh`,
which pulls commit subjects from `git log <previous-tag>..HEAD` and drops
anything tagged `[chore]`. See the script header for the full release flow.

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
