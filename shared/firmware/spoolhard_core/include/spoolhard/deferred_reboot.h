#pragma once
#include <stdint.h>

// Reboot from a detached one-shot task after `delay_ms`.
//
// For use inside AsyncWebServer handlers: req->send() only QUEUES the
// response on the AsyncTCP task — the same task the handler runs on. An
// inline delay()+ESP.restart() therefore stalls the TCP stack and the
// response never leaves the device; the client hangs on a dead socket
// until its own timeout. Returning immediately and rebooting from a
// separate task lets the queued response flush first.
//
// Falls back to a blocking delay+restart if the task can't be created
// (heap exhaustion) — a reboot is the one thing that must not silently
// fail to happen.
void spoolhardDeferredReboot(uint32_t delay_ms = 1000);
