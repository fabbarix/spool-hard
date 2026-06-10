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
**both devices flash over WiFi OTA** — neither is USB-attached.

- Console (`spuletto.local` / 192.168.20.153): multipart POST to
  `/api/upload/firmware` + `/api/upload/spiffs` with `?key=$SPULETTO_KEY`,
  bins from `console/firmware/.pio/build/esp32-s3/`.
- Scale (`scalinata.local` / 192.168.20.154): same endpoints with
  `?key=$SCALINATA_KEY`, bins from `scale/firmware/.pio/build/esp32-s3/`.
- CAUTION: whatever enumerates as `/dev/ttyACM0` (CH343 UART adapter) is
  some OTHER ESP32-S3 board, NOT the console or scale. Don't flash it.

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

## Local secrets — `.env` at repo root

`.env` (gitignored) holds local credentials needed to talk to the running
devices. Source it or read keys directly:

- `SPULETTO_KEY` — fixed-key for the **console** at `192.168.20.153`
  (hostname `spuletto.local`). Used as `Authorization: Bearer <KEY>` or
  `?key=<KEY>` against `/api/*` endpoints.
- `SCALINATA_KEY` — fixed-key for the **scale** at `192.168.20.154`
  (hostname `scalinata.local`). In typical setups this is the same key as
  the console.
- `BAMBU_PRINTER_IP` / `BAMBU_PRINTER_SERIAL` / `BAMBU_PRINTER_ACCESS_CODE`
  — paired Bambu printer's LAN address, serial, and access code.
- `ACCESS_TOKEN` — Bambu Cloud bearer token for `tools/bambu_login.py`
  / `tools/get_resources.py`.

Typical use:

```bash
export $(grep -v '^#' .env | xargs)
curl -H "Authorization: Bearer $SCALINATA_KEY" http://192.168.20.154/api/logs
```

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
  flashes firmware then frontend with sha256 verification, and
  `otaTaskSpawn()` which lifts the run onto its own FreeRTOS task so
  the caller's loop keeps running through the multi-minute fetch.
- `spoolhard/product_signature.h` — `SPOOLHARD-PRODUCT=<id>` byte marker
  + `ProductSignatureMatcher` for streaming-upload validation.
- `spoolhard/version_marker.h` / `src/version_marker.cpp` —
  `SPOOLHARD-VERSION=<v>\x01` byte marker + `VersionMarkerParser` that
  pulls the version out of an uploaded firmware image.
- `spoolhard/backup.h` / `src/backup.cpp` — `/api/backup` + `/api/restore`
  serializer covering NVS namespaces + on-disk files.
- `spoolhard/ring_log.h` / `src/ring_log.cpp` — bounded in-RAM log ring
  + `dlog(tag, fmt, ...)` printf-style helper. Backs `/api/logs`.
- `spoolhard/serial_mirror.h` / `src/serial_mirror.cpp` — `Serial`
  macro override that mirrors every `Serial.print*` to the ring log.
  Include AFTER all framework + project headers in each TU.
- `spoolhard/psram_json_alloc.h` / `src/psram_json_alloc.cpp` — `g_psramJsonAlloc`
  singleton: `JsonDocument doc(&g_psramJsonAlloc);` routes the node
  tree to PSRAM. All hot WS/HTTP paths use this.
- `spoolhard/ws_buffer_pool.h` / `src/ws_buffer_pool.cpp` — `g_wsBufPool`
  singleton: 8 × 8 KB pre-allocated `AsyncWebSocketSharedBuffer` slots.
  Eliminates internal-DRAM churn on broadcastDebug/broadcastState pushes.
- `spoolhard/auth.h` / `src/auth.cpp` — `requireAuth`, `wsAuthHandshake`,
  `handleAuthStatus`, `handleTestKey`, `handleFixedKeyConfigPost`,
  `handleDeviceName{Get,Post}`. Shared NVS schema: namespace `wifi_cfg`
  with keys `ssid`, `pass`, `device_name`, `fixed_key`.
- `spoolhard/common_routes.h` / `src/common_routes.cpp` — `registerAll(server)`
  installs `/api/restart` + `/api/logs` + `/api/heap` (heap_caps stats
  for DRAM-budget work). Both products call this once in `_setupRoutes`.
  (`/api/logs/current` + `/api/logs/previous` are console-specific —
  SD-persisted output.) NB: the esp32async fork's default route matcher
  is prefix-style; `/api/logs` uses `AsyncURIMatcher::exact()` so it
  doesn't shadow those sub-routes. Register specific-before-generic.
- `spoolhard/deferred_reboot.h` / `src/deferred_reboot.cpp` —
  `spoolhardDeferredReboot()`: reboot from a detached task so an
  AsyncWebServer handler's queued response actually flushes first.
  Never `delay()+ESP.restart()` inline in a handler.
- `spoolhard/psram_task.h` / `src/psram_task.cpp` — one-shot task
  spawner that would prefer a PSRAM stack, but the PSRAM path is
  compiled out: this framework's sdkconfig lacks
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` and FreeRTOS PANICS on
  external stack buffers without it. Read the header before touching.
- `spoolhard/panic_persist.h` / `src/panic_persist.cpp` — best-effort
  crash-log persistence without an esp_core_dump partition. On boot,
  if `esp_reset_reason()` indicates panic/WDT/brownout AND a pending
  ring-log tail file exists, it's promoted to `/crash_<seq>.txt`. The
  pending file is rewritten every 30 s during normal operation. Each
  product registers its own `/api/crashes` route to surface the files.

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

## Scale firmware task model (0.8.0+)

The scale used to run everything serially in `loop()`. As of 0.8.0
sensors and slow operations are split into dedicated FreeRTOS tasks
so the coordinator loop never blocks on hardware I/O:

| Task            | Stack | Core | Owns                                       |
|-----------------|-------|------|--------------------------------------------|
| `loopTask`      | Arduino default | 1 | Coordinator: drains task event queues, dispatches WS broadcasts, runs the LED state machine, services WiFi state machine. |
| `sensor_task`   | 4 KB  | 1    | `g_scale` (LoadCell). 100 Hz poll, drains command queue (tare/calibrate/addCalPoint/clear/reloadParams). Publishes a seqlock snapshot. |
| `nfc_task`      | 4 KB  | 1    | `g_nfc` (NfcReader / PN532 SPI). 50 Hz poll, drains write/erase/emulate command queue. Publishes a seqlock snapshot. |
| `console_tx`    | 4 KB  | 1    | Single writer to the port-80 `/ws/console` AsyncWebSocket. Drains a 32-deep frame queue. |
| `ota_run`       | 12 KB | 0    | Spawned on demand by `otaTaskSpawn`; self-deletes when done. Handles HTTPS manifest + binary fetch, sha256, `Update.write`. |
| `AsyncTCP_task` | system | 0   | mathieucarbou's lib + lwIP + WiFi. |

**Cross-task communication rules:**

- Single writer per resource. `g_scale` is mutated only from
  `sensor_task`; reads from anywhere else go through `SensorTask::*`
  accessors which are seqlock-safe. Same for `g_nfc` /
  `NfcTask::*`. Outbound WS frames go through `ConsoleTx::send()`,
  which the dedicated sender task drains.
- Bounded queues, drop-on-overflow. No queue grows without bound; if
  a queue is full, the producer logs a warning and discards. The
  sample/event loops are idempotent — the next tick always carries
  the latest authoritative state.
- Atomics for cross-task flags. `g_pendingOta`, `g_pendingPushHandshake`,
  `g_pendingPushPending`, `g_uploadActiveUntil`, `g_haveLastSuccessTag`
  are `std::atomic<>` because they're set from AsyncTCP callbacks and
  consumed by `loopTask`. Use `.exchange(false)` when reading-and-clearing
  to avoid TOCTOU windows.
- `ConsoleToScale::g_queue` is mutex-guarded (FreeRTOS semaphore). The
  inbound parser (`deliver`) runs on AsyncTCP; the dispatcher
  (`receive`) runs on `loopTask`.

**Single web server**: as of 0.8.0 there's only one `AsyncWebServer`
on port 80. `/ws` serves the browser dashboard, `/ws/console` serves
the paired console link. The retired port-81 server lives on in
`SCALE_WS_PORT_LEGACY` / `SCALE_WS_PATH_LEGACY` for older scale
firmware fallback (none deployed today).

**HX711 hardware rate**: `HX711_HW_RATE_HZ` in `scale/firmware/include/config.h`
documents whether the chip's RATE pin is wired for 80 Hz vs the 10 Hz
default. The `sensor_task` polls at 100 Hz unconditionally; the
constant is informational + drives the default `STABLE_COUNT_REQ`.

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
