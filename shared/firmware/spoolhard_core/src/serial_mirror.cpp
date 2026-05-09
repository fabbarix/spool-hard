// Don't redirect Serial in this TU — we need the real USB-CDC.
#define SERIAL_MIRROR_NO_OVERRIDE
#include "spoolhard/serial_mirror.h"
#include "spoolhard/ring_log.h"

LineBufferingPrint g_serial;

// Minimal-stack-footprint wrapper. Earlier revs had a global String
// accumulator + FreeRTOS mutex + per-byte loop; that pushed the FTPS
// analyzer task (16 KB stack, mbedtls handshake using ~10 KB of it)
// over its limit and bootlooped during gcode-body parse. Each
// Serial.printf already buffers its formatted output and calls write()
// once, so we just take that buffer as-is and hand it to RingLog::push
// (which has its own mutex + bounded String storage). USB-CDC mirror is
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

    // RingLog::push takes a String — one allocation per ring entry. That
    // String lives inside RingLog's bounded deque; once 80 entries are
    // in, the oldest is freed on each push. push() also throttles when
    // free internal heap drops below ~24 KB so logging can never tip a
    // marginal real allocation into OOM.
    String line;
    line.reserve(n);
    for (size_t i = 0; i < n; ++i) line += (char)buffer[i];
    RingLog::push(line);
    return size;
}
