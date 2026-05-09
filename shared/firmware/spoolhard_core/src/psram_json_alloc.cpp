#include "spoolhard/psram_json_alloc.h"
#include <esp_heap_caps.h>
#include <cstring>

PsramJsonAllocator g_psramJsonAlloc;

void* PsramJsonAllocator::allocate(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_DEFAULT);
    return p;
}

void PsramJsonAllocator::deallocate(void* p) {
    heap_caps_free(p);
}

void* PsramJsonAllocator::reallocate(void* p, size_t n) {
    // ArduinoJson grows nodes monotonically during deserialize; we
    // can't realloc PSRAM directly, so emulate via alloc + memcpy +
    // free. Old size comes from heap_caps_get_allocated_size — the
    // caller doesn't tell us how much of the buffer was in use.
    if (!p) return allocate(n);
    size_t old = heap_caps_get_allocated_size(p);
    void* np = allocate(n);
    if (!np) return nullptr;
    std::memcpy(np, p, old < n ? old : n);
    heap_caps_free(p);
    return np;
}
