// Don't redirect Serial in this TU — we need the real USB-CDC.
#define SERIAL_MIRROR_NO_OVERRIDE
#include "spoolhard/serial_mirror.h"
#include "spoolhard/ring_log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

LineBufferingPrint g_serial;

// Minimal-stack-footprint wrapper. Earlier revs had a global String
// accumulator + FreeRTOS mutex + per-byte loop; that pushed the FTPS
// analyzer task (16 KB stack, mbedtls handshake using ~10 KB of it)
// over its limit and bootlooped during gcode-body parse. Each
// Serial.printf already buffers its formatted output and calls write()
// once, so we just take that buffer as-is and hand it to RingLog::push
// (which has its own mutex + preallocated fixed slots, no per-call heap). USB-CDC mirror is
// gated on availableForWrite to avoid blocking when no host is opened.

static inline bool _serialHasRoom(size_t n) {
    return ::Serial && ::Serial.availableForWrite() >= (int)n;
}

void LineBufferingPrint::begin(unsigned long baud) {
    ::Serial.begin(baud);
}

int LineBufferingPrint::availableForWrite() {
    return ::Serial ? ::Serial.availableForWrite() : 256;
}

void LineBufferingPrint::flush() {
    // No-op — every write() pushes immediately, nothing pending.
}

size_t LineBufferingPrint::write(uint8_t c) {
    return write(&c, 1);
}

size_t LineBufferingPrint::write(const uint8_t* buffer, size_t size) {
    if (!buffer || size == 0) return 0;

    // Mirror to USB-CDC first, gated so a saturated / no-host TX never
    // blocks the calling task.
    if (_serialHasRoom(size)) {
        ::Serial.write(buffer, size);
    }

    // Strip trailing CR/LF in-place by computing a shortened length —
    // avoids constructing a String just to trim it. Skip empty payloads
    // (e.g. the standalone "\r\n" emitted by Serial.println after the
    // body of the same line).
    size_t n = size;
    while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) --n;
    if (n == 0) return size;

    // Fixed-slot ring: copies into a preallocated slot, no heap use.
    RingLog::push((const char*)buffer, n);
    return size;
}

size_t LineBufferingPrint::printf(const char* fmt, ...) {
    // Print::printf mallocs for output > 63 chars (internal-DRAM churn on
    // every long log line). Format into a bounded stack buffer instead and
    // truncate rather than allocate.
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return 0;
    if ((size_t)len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
        memcpy(buf + len - 4, "...\n", 4);
    }
    write((const uint8_t*)buf, (size_t)len);
    return (size_t)len;
}

size_t LineBufferingPrint::printf(const __FlashStringHelper* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), (const char*)fmt, ap);
    va_end(ap);
    if (len < 0) return 0;
    if ((size_t)len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
        memcpy(buf + len - 4, "...\n", 4);
    }
    write((const uint8_t*)buf, (size_t)len);
    return (size_t)len;
}
