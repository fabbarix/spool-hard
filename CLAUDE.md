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

## Code navigation & editing — use Serena MCP

This repo has the **Serena** MCP server attached. Prefer Serena's
symbol-aware tools over `grep`/`Read`/`Edit` for any task that involves
finding or modifying C/C++/JS/TS symbols. Serena uses the language
server, so it understands scope, overloads, and cross-file references —
which matters here because the protocol enums and OTA helpers are
mirrored across `console/`, `scale/`, and `shared/firmware/`.

**Discovery (read-only, cheap):**
- `mcp__serena__get_symbols_overview` — first call when opening an
  unfamiliar file; returns the top-level symbol tree without reading the
  body. Use this instead of `Read` for orientation.
- `mcp__serena__find_symbol` — locate a class/function/method by
  `name_path` (e.g. `ConsoleToScale/send`, or `/OtaConfig` for an
  absolute match). Pass `include_body=true` only when you need the
  source; pass `depth=1` to list members of a class without their
  bodies. Use `relative_path` to scope to one file/dir — much faster.
- `mcp__serena__find_referencing_symbols` — before renaming or deleting
  a symbol, or when adding a new protocol message and you need to find
  every `nameToType` / `typeToString` / `send` switch that has to be
  updated in lockstep.

**Editing:**
- `mcp__serena__replace_symbol_body` — swap the body of a known
  function/method without re-reading the surrounding file. Body must
  start at the signature line and exclude leading comments/imports.
- `mcp__serena__insert_after_symbol` / `insert_before_symbol` — add a
  new enum value, method, or include without hand-counting line numbers.
- `mcp__serena__rename_symbol` — repo-wide rename via the language
  server (handles call sites). Use this instead of `Edit ... replace_all`
  for anything that crosses files.
- `mcp__serena__safe_delete_symbol` — refuses to delete if references
  exist; returns the reference list so you know what to clean up first.

**When NOT to use Serena tools:**
- Plain text files (`VERSION`, `*.md`, `*.json`, `platformio.ini`,
  `CMakeLists.txt`) — use `Read` / `Edit` / `Write`.
- Whole-file rewrites or new-file creation — use `Write`.
- Looking for a literal string (URL, log message, config key) — use
  `grep` via `Bash`. Serena indexes symbols, not arbitrary text.
- One-off reads where you already know the exact lines you need —
  `Read` with `offset`/`limit` is fine.

**Serena's project memory** (`mcp__serena__{list,read,write}_memory`)
is separate from the `~/.claude/projects/.../memory/` auto-memory above.
Don't duplicate; only write a Serena memory if a fact is specifically
useful to future Serena symbol-navigation sessions (e.g. "the protocol
enum lives at `*/firmware/src/protocol.h`").
