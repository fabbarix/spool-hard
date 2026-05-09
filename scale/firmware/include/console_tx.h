#pragma once
#include <Arduino.h>

// Single-writer WebSocket sender for the port-81 console link.
//
// Protocol emit helpers (`ScaleToConsole::send`, `sendSimple`, etc.)
// previously called `g_console.sendText()` from whatever task they ran
// on — coordinator (main loop), sensor_task, nfc_task, ota_task,
// AsyncTCP. mathieucarbou's AsyncWebSocket::textAll has internal
// locking, but the JSON serialization that precedes it built up
// multi-KB Strings on whichever stack happened to be live; OTA task
// in particular has a tight stack budget after mbedtls.
//
// Now: every emit pushes a heap-allocated `String*` onto a bounded
// queue. A dedicated `console_tx` task drains the queue and dispatches
// to AsyncWebSocket. Stack lives in one place (4 KB), and we can
// centralise drop-on-overflow telemetry.
namespace ConsoleTx {

// Spawn the sender task. Idempotent. Call once at boot.
void begin();

// Hand off `frame` to the queue. Returns false if the queue is full —
// caller should treat it as a non-fatal drop. The frame is consumed
// (moved into a heap-allocated String); caller's local goes out of
// scope as normal.
//
// Safe to call from any task (including AsyncTCP), with no implicit
// blocking — `xQueueSend` with a short timeout.
bool send(const String& frame);

// Telemetry. Free slots = queue space available. Total = queue depth.
size_t free();
size_t total();
uint32_t framesTx();
uint32_t framesDropped();

}  // namespace ConsoleTx
