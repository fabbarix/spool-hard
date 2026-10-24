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
// Requests are identified by an opaque owner pointer (the request) so a
// rejected/unauthenticated second upload can't write into the active one.
namespace SpiffsUpload {

enum class Phase : uint8_t { Idle, Writing, Verifying, Ok, Failed };

// Size of the SPIFFS data partition per the partition table (0 = none).
size_t partitionSize();

// Start an upload. Returns nullptr on success, else a short static error
// string (no quotes — safe to embed in JSON). `imageSize` 0 = whole partition.
const char* begin(const void* owner, size_t imageSize = 0);

// Optional expected sha256 (64 hex chars) of the uploaded image bytes.
void setExpectedSha256(const void* owner, const char* hex);

// Append bytes. Returns false (and latches an error) on a short write.
bool write(const void* owner, const uint8_t* data, size_t len);

// True while `owner` holds the active (Writing) upload.
bool owns(const void* owner);

// Latch a failure (e.g. wrong product signature); finish() will clean up.
void fail(const void* owner, const char* reason);

// Async finish: spawns the verify task and returns immediately. On success
// reboots (deferred) when `reboot` is true.
void finish(const void* owner, bool reboot);

// Blocking finish for callers already on their own task (otaRun).
// Returns true iff the written image verified. Never reboots.
bool finishBlocking(const char* expectedSha256Hex);

// Abort an in-progress upload on the caller's task (format + remount).
void abortBlocking(const char* reason);

Phase       phase();
const char* error();                    // latched error of the last upload
bool        failed(const void* owner);  // error latched for this owner's upload
bool        isOwner(const void* owner); // owner of the most recent upload

// FS lock for SPIFFS writers other than the uploader. Non-blocking;
// returns false while an upload owns the filesystem. Pair with unlockFs().
bool tryLockFs();
void unlockFs();

}  // namespace SpiffsUpload
