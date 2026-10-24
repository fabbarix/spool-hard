#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <vector>

// Configuration backup + restore for SpoolHard firmwares.
//
// Each product (console / scale) drives this with its own list of NVS
// namespaces and filesystem mounts; the shared logic enumerates the
// requested NVS keys via ESP-IDF's nvs_entry_find, walks each FS root
// recursively, and emits an archive that can be downloaded by the user
// and re-uploaded on the same (or a sibling) device to restore the
// entire configured state.
//
// Wire format (current, "v2"): a plain POSIX ustar archive, streamed in
// both directions so memory use is bounded by one chunk regardless of
// how big the spool database grows (`tar tf backup.tar` to inspect):
//
//   manifest.json            {"spoolhard_backup":2,"format":"tar",
//                             "product","device_name","fw_version",
//                             "created_at"}  — always the first member
//   nvs.json                 {"<ns>":[{"k","t","v"},...],...}
//   <fs label>/<path>        raw file bytes, e.g. sd/spoolease/spools.jsonl,
//                            littlefs/spools.jsonl
//
// Restore streams each file member to `<path>.restore`, and only once
// the whole archive has arrived intact are NVS keys applied and the
// temp files renamed over the originals — a truncated upload changes
// nothing.
//
// Legacy wire format ("v1", still accepted on restore for uploads up
// to 256 KB; no longer produced — building it in RAM silently dropped
// large files once the spool DB outgrew PSRAM). A single JSON document:
//
//   {
//     "spoolhard_backup": 1,                  // schema version
//     "product":          "console",          // matches PRODUCT_ID
//     "device_name":      "Spuletto",
//     "fw_version":       "0.3.0",
//     "created_at":       1776916000,
//     "nvs": {
//       "wifi_cfg": [
//         {"k":"ssid",        "t":"str", "v":"MyWifi"},
//         {"k":"pass",        "t":"str", "v":"<secret>"},
//         {"k":"device_name", "t":"str", "v":"Spuletto"}
//       ],
//       "ota_cfg": [
//         {"k":"url",   "t":"str", "v":"https://..."},
//         {"k":"ck_en", "t":"u8",  "v":1},
//         {"k":"ck_hrs","t":"u32", "v":24}
//       ]
//     },
//     "files": [
//       {"fs":"littlefs","path":"/spools.jsonl",
//        "size":1234,"encoding":"base64","content":"..."},
//       {"fs":"sd","path":"/user_filaments.jsonl",
//        "size":567,"encoding":"base64","content":"..."}
//     ]
//   }
//
// "t" field (NVS value type) mirrors the ESP-IDF nvs_type_t enum:
//   u8 / u16 / u32 / u64   — unsigned ints
//   i8 / i16 / i32 / i64   — signed ints
//   str                    — null-terminated string
//   blob                   — opaque bytes; "v" is base64
//
// SECURITY: backup files contain the device's full secret state —
// WiFi password, fixed bearer key, Bambu Cloud token, scale-link
// shared secrets, printer access codes. Treat the file accordingly.
namespace SpoolhardBackup {

// One filesystem mount the backup should walk. The `label` becomes
// the value of the "fs" field on the wire and the keying for restore;
// "littlefs" and "sd" are the canonical labels we use today.
struct FsMount {
    const char* label;
    FS*         fs;
    // Files larger than this are skipped on backup. 0 = no cap. Keeps
    // bulk read-only assets (e.g. a 32KB filaments DB) out of the
    // user-config bundle. Set per-mount because the per-FS data
    // characteristics differ.
    size_t      max_file_size = 0;
    // Path prefixes to skip on backup (e.g. "/cache", "/tmp"). Match
    // is "starts with" — entries here cover both the file itself and
    // any directory whose name begins with the prefix.
    std::vector<const char*> skip_prefixes;
    // Filename suffixes to skip on backup (e.g. ".tmp"). "*.restore"
    // leftovers from an aborted restore are always skipped.
    std::vector<const char*> skip_suffixes;
};

// Describes which NVS namespaces and which filesystems to include.
struct Source {
    std::vector<const char*> nvs_namespaces;
    std::vector<FsMount>     fs_mounts;
};

// Stream a v2 (tar) backup as the response to `req`. Walks the mounts
// and snapshots NVS up front (metadata + a few KB of NVS JSON); file
// bytes are read from disk chunk-by-chunk as the client drains the
// socket. `filename` becomes the Content-Disposition download name.
void sendTarBackup(AsyncWebServerRequest* req,
                   const Source&          src,
                   const String&          product,
                   const String&          device_name,
                   const String&          fw_version,
                   const String&          filename);

// Build a v1 backup document (legacy; kept for tests/tools — the HTTP
// handlers use sendTarBackup). Returns false only on a hard internal
// failure (NVS partition unreadable, etc); per-mount/file errors are
// noted in `errors_out` but don't fail the whole operation. The
// document is shaped per the comment above and ready to be
// serializeJson'd into the HTTP response body.
bool buildBackup(const Source& src,
                 const String& product,
                 const String& device_name,
                 const String& fw_version,
                 JsonDocument& out,
                 String*       errors_out = nullptr);

// Apply a previously-built backup document. Restores every
// recognized NVS key + every file payload. Unknown NVS namespaces in
// the backup that aren't listed in `src.nvs_namespaces` are skipped
// (defensive — refuse to write into a namespace the product doesn't
// claim ownership of). File entries with an `fs` label not matching
// any mount in `src.fs_mounts` are also skipped.
//
// Restore does NOT reboot — caller decides when. Most callers will
// `delay(800); ESP.restart();` so the response can flush first.
struct RestoreReport {
    int    nvs_keys_set     = 0;
    int    nvs_keys_skipped = 0;
    int    files_written    = 0;
    int    files_skipped    = 0;
    int    errors           = 0;
    String first_error;       // populated on first failure for debugging
};
bool applyRestore(const Source&         src,
                  const JsonDocument&   doc,
                  RestoreReport&        out);

// Validation helper used by the restore handler before applyRestore.
// Returns true and leaves `reason` empty on success. On failure
// returns false and writes a short human-readable explanation:
//   - "not a SpoolHard backup"
//   - "wrong product (expected console, got scale)"
//   - "schema version N not supported"
bool validate(const JsonDocument& doc,
              const String&       expected_product,
              String&             reason);

// Streaming restore for /api/restore. Feed it the raw request body from
// the AsyncWebServer body callback, then call finish() from the final
// request handler. Detects the format from the first byte: '{' is a v1
// JSON backup (buffered, 256 KB cap), anything else must be a v2 tar.
//
// One session per server; a new begin() discards any previous state
// (and its temp files).
class RestoreSession {
public:
    RestoreSession() = default;
    ~RestoreSession();
    RestoreSession(const RestoreSession&) = delete;
    RestoreSession& operator=(const RestoreSession&) = delete;

    // index == 0 of a new upload. `src` must outlive the session's use
    // (products keep it as a member or static).
    void begin(const Source* src, const String& expected_product);
    // Mark the upload rejected (e.g. auth failed) — further feed()s are
    // ignored and finish() returns `http_status` with `reason`.
    void reject(const String& reason, int http_status = 400);
    void feed(const uint8_t* data, size_t len, size_t index, size_t total);
    // Validate + apply. Returns the HTTP status to send (200 ok, 4xx bad
    // upload, 500 apply failure); `rep` is filled either way and `error`
    // carries the reason on 4xx. Resets the session afterwards.
    int  finish(RestoreReport& rep, String& error);

private:
    struct Impl;
    Impl* _impl = nullptr;
};

}  // namespace SpoolhardBackup
