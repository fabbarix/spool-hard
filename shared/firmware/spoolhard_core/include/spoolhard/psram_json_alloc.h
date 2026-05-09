#pragma once
#include <ArduinoJson.h>

// PSRAM-preferring ArduinoJson allocator. Routes all JSON node-tree
// allocations into the ESP32's external SPI RAM (where we have ~8 MB
// to play with) instead of internal DRAM (~330 KB total, perpetually
// contested by lwIP / AsyncTCP / mbedtls / LVGL). Fallback to
// MALLOC_CAP_DEFAULT if PSRAM is exhausted so we never regress to
// returning nullptr on a transient PSRAM-full condition.
//
// Originally lived as `s_psramJsonAlloc` in bambu_printer.cpp; lifted
// here so the WS broadcast path can share the same instance and stop
// pressure-cooking internal DRAM with per-message alloc/free churn.
class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t n) override;
    void  deallocate(void* p) override;
    void* reallocate(void* p, size_t n) override;
};

// Singleton, initialised at first use. Safe to take its address from
// any task — no internal mutable state.
extern PsramJsonAllocator g_psramJsonAlloc;
