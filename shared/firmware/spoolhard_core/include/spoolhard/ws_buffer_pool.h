#pragma once
#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

// Pre-allocated `AsyncWebSocketSharedBuffer` pool. Eliminates the
// per-broadcast `make_shared<std::vector<uint8_t>>` in mathieucarbou's
// `AsyncWebSocket::makeSharedBuffer()` — which was the dominant
// internal-DRAM allocation site for the state.* push pipeline (5–10
// pushes/s while a tab is open, each one a ~1.5 KB malloc/free cycle).
//
// Lifecycle: at `begin()` we make() N shared_ptrs to vectors with
// `reserve(initial_capacity)` already paid. The pool keeps an owning
// ref to each. Callers `acquire()` to get a copy of one whose use
// count is 1 (= only the pool holds it, so it's safe to mutate).
// They serialize their payload into `*buf`, call `textAll(buf)`, and
// drop their local copy. The lib copies the shared_ptr into each
// client's queue — `use_count()` rises by `N+1`. When clients drain
// the queue, refcount falls back to 1 (pool only) and the buffer
// becomes acquirable again.
//
// If a buffer needs to grow past its initial capacity the underlying
// std::allocator falls back to internal DRAM (one-time growth, then
// the new capacity is pinned for the firmware's lifetime). Pre-
// reserving 8 KB per slot covers every state.* envelope we send today
// with margin, so growth essentially never happens in practice.
class WsBufferPool {
public:
    // count: how many slots to pre-allocate. 4 is plenty for a
    //        single browser tab + a slow client absorbing a flush —
    //        if all 4 are simultaneously in flight the next
    //        `acquire()` returns null and the broadcast is dropped
    //        (the next one will land — state.* is idempotent).
    // initial_capacity: bytes pre-reserved per slot's vector.
    void begin(size_t count = 4, size_t initial_capacity = 8192);

    // Returns a shared_ptr whose pointee is safe to mutate (only the
    // pool + the caller hold references). The caller is expected to
    // pass it to `_ws.textAll(buf)` immediately. If the buffer needs
    // to grow past its current capacity the vector reallocates on
    // its own — fine because the caller is the only mutator.
    //
    // Returns an empty shared_ptr if every slot is currently in a
    // client's send queue. Caller should `if (!buf) return;` and
    // skip the broadcast.
    AsyncWebSocketSharedBuffer acquire();

    // Telemetry — surface how many slots are free + busy via
    // /api/firmware-info so we can spot pool starvation.
    size_t totalSlots() const { return _slots.size(); }
    size_t freeSlots();

    // High-water instrumentation (lever C). Vectors only ever grow, so a
    // slot's current capacity() is the largest frame it has ever held —
    // scanning all slots tells us the real max frame size and how many
    // slots ever had to grow past `initial_capacity`. That's the data
    // needed to right-size the pool (cut initial_capacity / slot count)
    // without guessing. `psramSlots` reports how many slots' backing
    // store currently lives in PSRAM — ground-truth for whether the
    // operator-new override (lever D4) actually moved these buffers off
    // internal DRAM. Computed on demand under the mutex (few slots, cheap).
    size_t maxCapacity();    // largest capacity() across all slots, bytes
    size_t grownSlots();     // slots whose capacity() exceeds initial_capacity
    size_t psramSlots();     // slots whose backing store is in external RAM

private:
    std::vector<AsyncWebSocketSharedBuffer> _slots;
    SemaphoreHandle_t _mtx = nullptr;
    size_t _next = 0;
    size_t _initialCap = 0;
};

extern WsBufferPool g_wsBufPool;
