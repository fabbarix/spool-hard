#include "spoolhard/ring_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

namespace RingLog {

// Fixed slot ring: kMaxEntries slots of kMaxLineLen chars, allocated ONCE
// (first push) and never touched by the allocator again — push() does no
// per-call heap work, so logging causes zero internal-DRAM churn. Prefers
// PSRAM when present (console); on the scale (no PSRAM) it is one
// ~13.5 KB internal block taken early at boot, which is no more than the
// old deque<String> used at steady state (80 x (node + String + heap)).
// Lines longer than kMaxLineLen-1 are tail-truncated and end in "...".
static constexpr size_t kMaxEntries = 80;
static constexpr size_t kMaxLineLen = 160;

struct Slot {
    uint32_t seq;
    uint32_t millis_at;
    uint8_t  len;
    char     text[kMaxLineLen + 1];
};

static Slot*     s_slots = nullptr;   // kMaxEntries, circular
static uint32_t  s_seq = 0;           // seq of newest entry (0 = none)
static SemaphoreHandle_t volatile s_mtx = nullptr;

static SemaphoreHandle_t _lazyMutex(SemaphoreHandle_t volatile* slot) {
    // Race-free lazy create: build a candidate, CAS it into the slot, and
    // delete it if another task won. Safe from any task/core, any time.
    SemaphoreHandle_t cur = *slot;
    if (cur) return cur;
    SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
    if (!fresh) return nullptr;
    SemaphoreHandle_t expected = nullptr;
    if (__atomic_compare_exchange_n((SemaphoreHandle_t*)slot, &expected, fresh, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return fresh;
    }
    vSemaphoreDelete(fresh);
    return expected;
}

static SemaphoreHandle_t _ensureMutex() { return _lazyMutex(&s_mtx); }

// Caller holds the mutex.
static bool _ensureSlots() {
    if (s_slots) return true;
    size_t bytes = sizeof(Slot) * kMaxEntries;
    void* p = nullptr;
    if (psramFound()) p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_slots = (Slot*)p;
    return s_slots != nullptr;
}

void push(const char* text, size_t len) {
    if (!text) return;
    // Strip trailing CR/LF.
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) --len;
    if (len == 0) return;
    SemaphoreHandle_t m = _ensureMutex();
    if (!m || xSemaphoreTake(m, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (_ensureSlots()) {
        Slot& sl = s_slots[s_seq % kMaxEntries];   // slot of seq+1 == (seq+1-1)
        bool trunc = len > kMaxLineLen;
        size_t n = trunc ? kMaxLineLen : len;
        memcpy(sl.text, text, n);
        if (trunc) memcpy(sl.text + n - 3, "...", 3);
        sl.text[n] = 0;
        sl.len = (uint8_t)n;
        sl.millis_at = millis();
        sl.seq = ++s_seq;
    }
    xSemaphoreGive(m);
}

void push(const String& line) { push(line.c_str(), line.length()); }

void forEach(uint32_t since, size_t limit,
             void (*fn)(void*, uint32_t, uint32_t, const char*), void* ctx) {
    SemaphoreHandle_t m = _ensureMutex();
    if (!m || xSemaphoreTake(m, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (s_slots && s_seq) {
        uint32_t oldest = s_seq > kMaxEntries ? s_seq - kMaxEntries + 1 : 1;
        uint32_t first  = since + 1 > oldest ? since + 1 : oldest;
        size_t emitted = 0;
        for (uint32_t q = first; q <= s_seq && emitted < limit; ++q, ++emitted) {
            const Slot& sl = s_slots[(q - 1) % kMaxEntries];
            fn(ctx, sl.seq, sl.millis_at, sl.text);
        }
    }
    xSemaphoreGive(m);
}

std::deque<Entry> snapshot(uint32_t since, size_t limit) {
    std::deque<Entry> out;
    forEach(since, limit, [](void* c, uint32_t seq, uint32_t ms, const char* t) {
        Entry e; e.seq = seq; e.millis_at = ms; e.line = t;
        ((std::deque<Entry>*)c)->push_back(std::move(e));
    }, &out);
    return out;
}

uint32_t headSeq() {
    _ensureMutex();
    uint32_t r = 0;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        r = s_seq;
        xSemaphoreGive(s_mtx);
    }
    return r;
}

}  // namespace RingLog

void dlog(const char* tag, const char* fmt, ...) {
    char buf[281];  // +1: room for the Serial newline
    int  n = 0;
    if (tag && *tag) {
        n = snprintf(buf, sizeof(buf), "[%s] ", tag);
        if (n < 0 || (size_t)n >= sizeof(buf) - 1) n = sizeof(buf) - 2;
    }
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - 1 - (size_t)n, fmt, args);
    va_end(args);
    (void)m;

    // Ring first — bounded 50ms mutex, never blocks long. CRITICAL:
    // Serial mirror MUST come second because USB CDC (Serial on
    // ESP32-S3 with ARDUINO_USB_CDC_ON_BOOT=1) blocks indefinitely
    // when no host is opened on /dev/ttyACM0 and the TX buffer
    // saturates. Calling Serial.printf first from the AsyncTCP task
    // would freeze the entire HTTP server until a USB host shows up
    // — which is exactly the bug that hid all dlog output during
    // remote debugging. Even with the order swap we additionally
    // gate the Serial mirror on a host-connected check so we never
    // block here at all when running headless.
    size_t blen = strlen(buf);
    RingLog::push(buf, blen);
    if (Serial && Serial.availableForWrite() >= (int)blen + 2) {
        // Not Serial.printf: Print::printf mallocs for lines > 63 chars.
        buf[blen] = '\n';
        Serial.write((const uint8_t*)buf, blen + 1);
    }
}
