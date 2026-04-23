#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <vector>

// Configuration backup + restore for SpoolHard firmwares.
//
// Each product (console / scale) drives this with its own list of NVS
// namespaces and filesystem mounts; the shared logic enumerates the
// requested NVS keys via ESP-IDF's nvs_entry_find, walks each FS root
// recursively, and emits one JSON document that can be downloaded by
// the user and re-uploaded on the same (or a sibling) device to
// restore the entire configured state.
//
// Wire format (a single JSON document):
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
};

// Describes which NVS namespaces and which filesystems to include.
struct Source {
    std::vector<const char*> nvs_namespaces;
    std::vector<FsMount>     fs_mounts;
};

// Build a backup document. Returns false only on a hard internal
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

}  // namespace SpoolhardBackup
