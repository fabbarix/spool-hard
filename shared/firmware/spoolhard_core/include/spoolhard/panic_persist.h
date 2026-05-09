#pragma once
#include <Arduino.h>

// Best-effort crash + reset-reason persistence — a software-only
// substitute for ESP-IDF's `esp_core_dump_to_flash` (which would need
// a partition-table change we're not making in this pass).
//
// What it does:
//
//   begin(fsBasePath)
//       Logs the previous reset reason via dlog so /api/logs surfaces
//       it. If the previous reason was anything panic-flavoured
//       (PANIC, INT_WDT, TASK_WDT, BROWNOUT) the prior boot's
//       partial ring-log tail (if it managed to persist) gets
//       renamed to a stable /crash_<seq>.txt for retrieval via
//       /api/crashes (each product registers its own handler for
//       that route).
//
//   markGracefulExit()
//       Call from ESP.restart() paths so the next-boot reset reason
//       is recognised as intentional and we don't false-positive a
//       "crash" file on benign reboots.
//
// Implementation notes:
//   - SPIFFS isn't safe to call from a panic context. We capture the
//     ring-log tail to a "pending" file at periodic intervals during
//     normal operation; on next boot we check whether the previous
//     reason was crashy and promote the tail to a stable seq if so.
//   - The pending file is small (~8 KB) so the SPIFFS write cost is
//     negligible. Persisted at most every 30 s and on clean
//     `markGracefulExit()` calls.
//   - On clean reboots the pending file is deleted to avoid
//     confusing the next-boot promotion check.
namespace PanicPersist {

// Initialise. Mounts SPIFFS must be done by the caller first.
// Returns the previous-boot reset reason as a short string ("PANIC",
// "POWERON", etc.) for the caller to log if useful.
const char* begin();

// Periodic tick — call once per loop iteration (cheap). Internally
// rate-gates SPIFFS writes to once every 30 s.
void tick();

// Mark this boot as cleanly exiting (call before ESP.restart()).
void markGracefulExit();

// Reset reason classification helper.
bool wasCrash(const char* reason);

}  // namespace PanicPersist
