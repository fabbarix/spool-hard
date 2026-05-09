#pragma once
#include "nfc_reader.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Single-owner FreeRTOS task wrapper around `g_nfc` (NfcReader).
//
// Why: PN532 SPI reads (especially the multi-page ntag2xx_ReadPage
// loop in `_readNdef`) can hold the bus for 80–160 ms per scan, and
// tag-write blocks even longer (~600 ms for a fully-written NTAG215).
// Doing this on the loopTask interleaves with the WS heartbeat and
// the load-cell sampler — a single tag scan can stretch one loop
// tick past 200 ms and trigger pong delays on the console link.
//
// Now: a dedicated `nfc_task` is the SOLE owner of the PN532. It
// polls for tags continuously, publishes status snapshots, and
// drains a command queue for write/erase/emulate. Blocking SPI
// I/O is contained inside this task; the rest of the system is
// unaffected.
namespace NfcTask {

// Atomic-publish snapshot. Mirrors SensorTask's seqlock pattern for
// multi-field consistency.
struct Snapshot {
    TagStatus status = TagStatus::Idle;
    SpoolTag  tag    = {};
};

// Spawn the task. Idempotent. Must be called after `g_nfc.begin()`.
// Stack 4 KB; priority 4 on core 1.
void begin();

// Read-only snapshot. Seqlock-safe.
Snapshot snapshot();

// Convenience getters — single-word reads, atomic.
TagStatus getStatus();

// Mutating commands. Return immediately; status updates flow through
// the snapshot + the event queue below.
struct WriteRequest {
    uint8_t uid[7];
    uint8_t uid_len;
    String  ndef_message;
    String  cookie;
};
struct EraseRequest {
    uint8_t uid[7];
    uint8_t uid_len;
};

void requestWrite(const WriteRequest& r);
void requestErase(const EraseRequest& r);
void requestEmulate(const String& url);

// Per-transition events drained by the coordinator. Returns false on
// empty queue. The event carries the FULL tag at the moment of the
// transition, so the coordinator can publish without a follow-up
// snapshot read.
struct StatusEvent {
    TagStatus new_status;
    SpoolTag  tag;
};
bool pollEvent(StatusEvent& out);

}  // namespace NfcTask
