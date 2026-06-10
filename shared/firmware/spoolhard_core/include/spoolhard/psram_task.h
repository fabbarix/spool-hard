#pragma once
#include <Arduino.h>

// One-shot FreeRTOS task whose stack lives in PSRAM when possible.
//
// Generalised from console/src/bambu_printer.cpp's spawnPSRAMFallbackTask.
// Each task type owns one SpoolhardPsramTaskSlot: the PSRAM stack buffer
// is lazy-allocated on first use and kept for the firmware's lifetime
// (xTaskCreateStatic never frees it; reuse avoids a leak). Concurrency
// within a type must be gated by the caller — the slot refuses reuse
// while `busy`.
//
// HARD CONSTRAINT: a task on a PSRAM stack must never touch internal
// flash — no NVS/Preferences (reads included), no SPIFFS/LittleFS, no
// Update.write. Flash operations run with the cache disabled, and a
// cache-disabled PSRAM stack access is an instant crash. Network, SD,
// String/heap work are all fine. On boards without PSRAM the SPIRAM
// alloc fails and the task falls back to a plain internal-stack spawn,
// so the constraint is only load-bearing where PSRAM exists.
//
// The task function must clear `slot->busy = false` (if it ran on the
// slot) before its terminal vTaskDelete(nullptr). Pass the slot through
// to the task via its arg if it can't see it otherwise.
struct SpoolhardPsramTaskSlot {
    StaticTask_t  tcb;
    StackType_t*  buf = nullptr;
    size_t        buf_bytes = 0;
    volatile bool busy = false;
};

// Spawn `fn` PSRAM-stack-first, falling back to a normal internal-stack
// task. Returns true on success; on the fallback path `slot.busy` stays
// false (the slot was never taken).
//
// NOTE: the PSRAM-stack path is compiled out unless the IDF sdkconfig
// sets CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY — without it FreeRTOS
// asserts (panics) on an external stack buffer. arduino-esp32's
// prebuilt qio_qspi config does NOT set it, so on today's framework
// this helper always lands on an internal stack; the API exists so
// call sites are ready if the framework ever gets rebuilt with it.
bool spoolhardSpawnPsramTask(TaskFunction_t fn, void* arg, const char* name,
                             size_t stack_bytes, UBaseType_t priority,
                             BaseType_t core_id, SpoolhardPsramTaskSlot& slot);
