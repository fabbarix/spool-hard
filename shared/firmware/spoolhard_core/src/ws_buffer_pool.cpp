#include "spoolhard/ws_buffer_pool.h"
#include <esp_heap_caps.h>
#include <soc/soc_memory_types.h>   // esp_ptr_external_ram

WsBufferPool g_wsBufPool;

void WsBufferPool::begin(size_t count, size_t initial_capacity, bool lazy) {
    if (_mtx) return;   // idempotent
    _mtx = xSemaphoreCreateMutex();
    _initialCap = initial_capacity;
    _count = count;
    _lazy = lazy;
    if (lazy) return;   // slots are created by acquire()
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
    if (_lazy && n < _count) {
        // Grow on demand, but only when no existing slot is idle and the
        // allocation leaves headroom. A refused alloc drops the push.
        bool anyIdle = false;
        for (auto& s : _slots) if (s.use_count() == 1) { anyIdle = true; break; }
        if (!anyIdle) {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (largest >= _initialCap + kLazyFloorBytes) {
                auto buf = std::make_shared<std::vector<uint8_t>>();
                buf->reserve(_initialCap);
                _slots.push_back(std::move(buf));
                n = _slots.size();
                _next = n - 1;
                uint32_t now = millis();
                if (_lastLogMs == 0 || now - _lastLogMs > 5000) {
                    _lastLogMs = now;
                    Serial.printf("[WsPool] slot allocated: total=%u ws_clients=%u\n",
                                  (unsigned)n, (unsigned)_wsClients.load());
                }
            } else {
                uint32_t now = millis();
                if (_lastWarnMs == 0 || now - _lastWarnMs > 60000) {
                    _lastWarnMs = now;
                    Serial.printf("[WsPool] alloc refused (largest free block %u) - push dropped\n",
                                  (unsigned)largest);
                }
            }
        }
    }
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

void WsBufferPool::maintain(size_t wsClientCount) {
    if (!_mtx || !_lazy) return;
    _wsClients.store((uint32_t)wsClientCount);
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (wsClientCount == 0) {
        size_t before = _slots.size();
        // Erase only slots nobody else references (use_count()==1); a slot
        // still queued in a dying client, or just handed to a caller, is
        // retried on the next tick.
        for (size_t i = 0; i < _slots.size();) {
            if (_slots[i].use_count() == 1) _slots.erase(_slots.begin() + i);
            else ++i;
        }
        if (_slots.size() != before)
            Serial.printf("[WsPool] freed %u slot(s), %u remain (busy), ws_clients=0\n",
                          (unsigned)(before - _slots.size()), (unsigned)_slots.size());
        if (_slots.empty()) std::vector<AsyncWebSocketSharedBuffer>().swap(_slots);
        _next = 0;
        _excessSinceMs = 0;
    } else {
        // Shrink: more slots than connected clients is surplus. Once it
        // has persisted kShrinkDelayMs (hysteresis vs. broadcast bursts),
        // erase idle (use_count()==1) slots down to the client count.
        if (_slots.size() > wsClientCount) {
            uint32_t now = millis();
            if (_excessSinceMs == 0) _excessSinceMs = now ? now : 1;
            else if (now - _excessSinceMs >= kShrinkDelayMs) {
                size_t before = _slots.size();
                for (size_t i = _slots.size(); i-- > 0 && _slots.size() > wsClientCount;)
                    if (_slots[i].use_count() == 1) _slots.erase(_slots.begin() + i);
                if (_slots.size() != before)
                    Serial.printf("[WsPool] shrunk %u -> %u slot(s), ws_clients=%u\n",
                                  (unsigned)before, (unsigned)_slots.size(),
                                  (unsigned)wsClientCount);
                if (_slots.size() <= wsClientCount) _excessSinceMs = 0;
                if (_next >= _slots.size()) _next = 0;
            }
        } else {
            _excessSinceMs = 0;
        }
        for (auto& s : _slots) {
            if (s.use_count() == 1 && s->capacity() > _initialCap) {
                std::vector<uint8_t>().swap(*s);
                s->reserve(_initialCap);
            }
        }
    }
    xSemaphoreGive(_mtx);
}
