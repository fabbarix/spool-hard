#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

// SD-backed log persistence + crash capture.
//
// Why: ring_log only lives in DRAM, so any crash hard-resets it before the
// user can pull it via /api/logs. We periodically spill the in-memory ring
// to /spoolease/logs/current.log on the SD card, and on boot — if the
// ESP-IDF reset reason indicates a panic / watchdog / brownout — preserve
// the previous session's log under /spoolease/crashes/ so the user can
// review what the firmware was doing right before it died.
//
// Flushing is gated to ~3 s in the steady state (cheap when no new ring
// entries) so SD wear stays well within tolerance even on chatty days.
namespace CrashLogger {

struct CrashEntry {
    uint32_t seq;          // numeric id, also the filename prefix
    String   reason;       // short reset-reason tag (panic, task_wdt, …)
    uint32_t size_bytes;   // file size on disk
    uint32_t mtime_unix;   // last-write time, 0 if SNTP wasn't ready
};

// Wire up after g_sd.begin(). If a crash reset is detected, copies the
// previous session's current.log to /spoolease/crashes/<seq>-<reason>.log,
// then truncates current.log and writes a fresh "=== boot ===" header.
// Cheap no-op when the SD card is missing.
void begin();

// Drains new RingLog entries to current.log. Internal interval gate, so
// calling every loop() tick is fine. Self-rotates current.log when it
// grows past the size cap.
void update();

// Force-flush whatever is in the ring buffer right now. Use from
// orderly shutdown handlers (esp_register_shutdown_handler) so a planned
// reboot doesn't lose the last few seconds of log.
void flush();

// True when SD is mounted and the persistence loop is running. Used by
// the API handler to surface a clear error when the user opens the
// debug tab and there's no card.
bool isAvailable();

// Crash-file management for /api/crashes.
std::vector<CrashEntry> list();
String                  pathFor(uint32_t seq);   // "" if seq doesn't exist
bool                    remove(uint32_t seq);
size_t                  removeAll();

// Path to the currently-being-written log. Useful for /api/logs/current
// (live tail of the on-disk log, separate from the in-memory ring).
const char* currentLogPath();

}  // namespace CrashLogger
