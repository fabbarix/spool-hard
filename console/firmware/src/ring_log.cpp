#include "ring_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstdio>

namespace RingLog {

// Cap at 200 entries × ~256 bytes ≈ 50 KB max. The deque drops oldest
// when full so the buffer is bounded regardless of how chatty
// dlog-using subsystems get. Increase only if you find you're losing
// logs faster than callers poll — with /api/logs polled every 2 s,
// 200 lines is several seconds of headroom.
static constexpr size_t kMaxEntries = 200;
static constexpr size_t kMaxLineLen = 256;

static std::deque<Entry> s_buf;
static uint32_t          s_seq = 0;
static SemaphoreHandle_t s_mtx = nullptr;

static void _ensureMutex() {
    // Lazy-init the mutex so callers don't need an explicit begin().
    // The first push lazily allocates; concurrent races at boot would
    // double-init but RingLog is a singleton on a single core so it's
    // a non-issue in practice.
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

void push(const String& line) {
    _ensureMutex();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;

    String trimmed = line;
    // Strip trailing newline + cap length. Long lines get tail-truncated
    // because the head usually carries the "what" + tag and is more
    // useful for grep.
    while (trimmed.length() && (trimmed.endsWith("\n") || trimmed.endsWith("\r"))) {
        trimmed.remove(trimmed.length() - 1);
    }
    if (trimmed.length() > kMaxLineLen) {
        trimmed = trimmed.substring(0, kMaxLineLen);
    }

    Entry e;
    e.seq       = ++s_seq;
    e.millis_at = millis();
    e.line      = std::move(trimmed);
    s_buf.push_back(std::move(e));
    while (s_buf.size() > kMaxEntries) s_buf.pop_front();

    xSemaphoreGive(s_mtx);
}

std::deque<Entry> snapshot(uint32_t since, size_t limit) {
    std::deque<Entry> out;
    _ensureMutex();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return out;
    for (auto& e : s_buf) {
        if (e.seq <= since) continue;
        out.push_back(e);
        if (out.size() >= limit) break;
    }
    xSemaphoreGive(s_mtx);
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
    char buf[280];
    int  n = 0;
    if (tag && *tag) {
        n = snprintf(buf, sizeof(buf), "[%s] ", tag);
        if (n < 0 || (size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    }
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, args);
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
    RingLog::push(String(buf));
    if (Serial && Serial.availableForWrite() >= (int)strlen(buf) + 2) {
        Serial.printf("%s\n", buf);
    }
}
