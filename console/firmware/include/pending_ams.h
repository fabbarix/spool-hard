#pragma once
#include <Arduino.h>

// One-shot "the next spool loaded on any AMS gets this spool_id pushed to it"
// state. Set by main.cpp when a known tag is scanned; consumed by BambuPrinter
// once any tray transitions from empty → populated (or swap to different
// populated state) within the expiry window.
//
// Rationale: tag events and AMS events arrive on completely different
// channels (console NFC / scale NFC vs. Bambu MQTT) and there's no reliable
// way to correlate them from first principles. The operator flow is "tap the
// tag, then physically load the spool" — so time-bounded optimistic pairing
// is the simplest thing that works.
namespace PendingAms {

// Default expiry window. Long enough to cover the user walking to the
// printer and loading the spool; short enough that a stray AMS change an
// hour later doesn't accidentally consume the pairing.
constexpr uint32_t kDefaultExpiryMs = 120000;  // 2 min

// Arm the pending assignment. Overwrites any existing pending.
void arm(const String& spool_id, uint32_t expiry_ms = kDefaultExpiryMs);

// Atomically consume the pending if armed and not expired. Returns true and
// writes the spool id into `out` on success; clears the pending so only one
// printer/tray wins the race.
bool claim(String& out_spool_id);

// Current pending spool id (or empty if nothing pending / expired). Does NOT
// consume.
String peek();

// Drop any pending state.
void clear();

}  // namespace PendingAms
