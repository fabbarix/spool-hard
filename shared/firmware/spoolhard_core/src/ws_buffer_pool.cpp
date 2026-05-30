#include "spoolhard/ws_buffer_pool.h"
#include <esp_heap_caps.h>
#include <soc/soc_memory_types.h>   // esp_ptr_external_ram

WsBufferPool g_wsBufPool;

void WsBufferPool::begin(size_t count, size_t initial_capacity) {
    if (_mtx) return;   // idempotent
    _mtx = xSemaphoreCreateMutex();
    _initialCap = initial_capacity;
    _slots.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto buf = std::make_shared<std::vector<uint8_t>>();
        buf->reserve(initial_capacity);
        _slots.push_back(std::move(buf));
    }
}

AsyncWebSocketSharedBuffer WsBufferPool::acquire() {
    if (!_mtx) return nullptr;   // begin() not called yet
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return nullptr;

    AsyncWebSocketSharedBuffer result;
    // Round-robin search starting at _next so a steady stream of
    // broadcasts cycles through all slots evenly (spreads any vector
    // growth across the pool rather than always exercising slot 0).
    size_t n = _slots.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (_next + i) % n;
        // use_count() == 1 means only the pool's slot owns the
        // shared_ptr. No client queue holds it; we can safely mutate.
        // We then `result = _slots[idx]` which copies the shared_ptr
        // (refcount → 2) — pool keeps its ref, caller takes one.
        if (_slots[idx].use_count() == 1) {
            result = _slots[idx];
            _next = (idx + 1) % n;
            break;
        }
    }
    xSemaphoreGive(_mtx);
    return result;
}

size_t WsBufferPool::freeSlots() {
    if (!_mtx) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t n = 0;
    for (auto& s : _slots) if (s.use_count() == 1) ++n;
    xSemaphoreGive(_mtx);
    return n;
}

size_t WsBufferPool::maxCapacity() {
    if (!_mtx) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t m = 0;
    for (auto& s : _slots) if (s && s->capacity() > m) m = s->capacity();
    xSemaphoreGive(_mtx);
    return m;
}

size_t WsBufferPool::grownSlots() {
    if (!_mtx) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t n = 0;
    for (auto& s : _slots) if (s && s->capacity() > _initialCap) ++n;
    xSemaphoreGive(_mtx);
    return n;
}

size_t WsBufferPool::psramSlots() {
    if (!_mtx) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t n = 0;
    // data() on a reserved-but-empty vector still points at the backing
    // allocation, so this is valid even before any frame is serialized in.
    for (auto& s : _slots)
        if (s && s->capacity() > 0 && esp_ptr_external_ram(s->data())) ++n;
    xSemaphoreGive(_mtx);
    return n;
}
