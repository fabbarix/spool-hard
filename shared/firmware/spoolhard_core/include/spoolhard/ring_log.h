#pragma once
#include <Arduino.h>
#include <deque>

// Tiny ring-buffered log so the web UI / curl can pull recent firmware
// output without an attached USB cable. Capture is selective (the dlog()
// helper below) — we don't hook the whole Arduino Serial path because
// it'd flood the buffer with framework noise on every loop tick.
//
// Each entry carries a monotonically-increasing sequence number so HTTP
// callers can poll with `?since=<seq>` and get only what's new — same
// pattern any tail-following client would use.
namespace RingLog {

struct Entry {
    uint32_t seq;        // monotonic id
    uint32_t millis_at;  // device millis() at push time
    String   line;
};

// Push one line. If the line ends in a newline it's stripped — the web
// layer adds delimiters when serialising. Safe to call from any task.
// No per-call heap allocation (fixed slot ring, lines > 159 chars are
// truncated and end in "...").
void push(const char* text, size_t len);
void push(const String& line);

// Allocation-free iteration over entries with seq > since (oldest first,
// at most `limit`). `fn` runs under the ring mutex: keep it short, don't
// call RingLog::* or block from it.
void forEach(uint32_t since, size_t limit,
             void (*fn)(void* ctx, uint32_t seq, uint32_t millis_at, const char* text),
             void* ctx);

// Snapshot up to `limit` entries with seq > `since`. Latest at the
// back. Returns the highest seq seen so the caller can use it as the
// next `since`.
std::deque<Entry> snapshot(uint32_t since, size_t limit = 200);

// Highest seq pushed so far (0 if nothing). Useful for "tail -f" style
// callers that want to discover the current head before subscribing.
uint32_t headSeq();

}  // namespace RingLog

// printf-style helper that mirrors Serial.printf AND pushes the
// formatted line into the ring. Tag is a short subsystem name shown
// at the front of every line (e.g. "CloudFil", "OTA"). Adds a trailing
// newline to Serial only — the ring entry stays unterminated.
void dlog(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
