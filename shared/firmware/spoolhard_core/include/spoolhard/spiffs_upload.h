#pragma once
#include <Arduino.h>

// Integrity-checked raw write of the SPIFFS (frontend) partition.
//
// Why this exists: Update.write() into U_SPIFFS bypasses the filesystem.
// Doing that while SPIFFS is mounted lets any SPIFFS writer (the scale's
// PanicPersist::tick every 30 s, /tags_in_store.txt) scribble over the new
// image, and ignoring Update.write() returns hides short writes. This
// module owns the whole sequence:
//
//   begin()    take the FS lock (PanicPersist & co. back off), SPIFFS.end(),
//              Update.begin(<partition size from the partition table>)
//   write()    checks every Update.write() return, hashes the stream
//   finish()   Update.end, optional client sha256 check, erase the tail of
//              the partition, read the partition back and compare the
//              sha256, remount SPIFFS and read every file end to end.
//              ANY failure => SPIFFS.format() + remount (clean empty FS),
//              error reported. Runs in a spawned task (never on AsyncTCP).
//
// Abandoned uploads (client vanished mid-stream) are detected by a
// 20 s no-progress timer and recovered the same way (format + remount).
//
// Uploads are identified by a generation token (never a raw request
// pointer, which can be reused), returned by begin().
//
// Product check before destruction: begin() only arms the upload and
// stages the first 4 KB in RAM. If the staged bytes contain a
// "SPOOLHARD-PRODUCT=" marker for a different product the upload is
// rejected with the old FS untouched. The FS is quiesced/erased only once
// the right marker is seen (mkspiffs puts it in the first file, ~offset 500)
// or 4 KB went by without any marker (then the final-chunk check in the
// handler remains the last line of defence, and a wrong image wipes the FS).
namespace SpiffsUpload {

enum class Phase : uint8_t { Idle, Writing, Verifying, Ok, Failed };

// Size of the SPIFFS data partition per the partition table (0 = none).
size_t partitionSize();

// Start an upload. Returns a nonzero token on success; on refusal returns 0
// and sets *err to a short static string (no quotes; JSON-safe).
// `productSig` e.g. "SPOOLHARD-PRODUCT=console" (nullptr = no early check).
// `imageSize` 0 = whole partition. `stallMs` 0 disables the abandon timer
// (callers already supervised, e.g. otaRun).
uint32_t begin(const char** err, const char* productSig = nullptr,
               size_t imageSize = 0, uint32_t stallMs = 20000);

// Optional expected sha256 (64 hex chars) of the uploaded image bytes.
void setExpectedSha256(uint32_t tok, const char* hex);

// Append bytes. Returns false (and latches an error) on failure.
bool write(uint32_t tok, const uint8_t* data, size_t len);

// True while `tok` is the active upload still accepting bytes.
bool owns(uint32_t tok);

// Latch a failure (e.g. wrong product at the final check).
void fail(uint32_t tok, const char* reason);

// Async finish: spawns the verify task and returns immediately.
void finish(uint32_t tok, bool reboot);

// Blocking finish for callers already on their own task (otaRun).
bool finishBlocking(uint32_t tok, const char* expectedSha256Hex);

// Abort on the caller's task (formats if the FS was already erased).
void abortBlocking(uint32_t tok, const char* reason);

Phase       phase();
const char* error();                  // latched error of the last upload
bool        failed(uint32_t tok);     // error latched for upload `tok`
bool        isCurrent(uint32_t tok);  // tok is the most recent upload

// FS lock for SPIFFS writers other than the uploader. Non-blocking;
// returns false while an upload owns the filesystem. Pair with unlockFs().
bool tryLockFs();
void unlockFs();

}  // namespace SpiffsUpload
