# Changelog

All notable changes to SpoolHard are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

New entries are appended automatically by `scripts/update_changelog.sh`,
which pulls commit subjects from `git log <previous-tag>..HEAD` and drops
anything tagged `[chore]`. See the script header for the full release flow.

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
