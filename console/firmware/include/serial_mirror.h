#pragma once

// Mirror everything written to `Serial` — Serial.printf, .print, .println,
// .write — into the dlog ring (see ring_log.h) so the web `/api/logs`
// endpoint surfaces it remotely. Source files opt in by including this
// header AFTER all framework + project headers; the trailing macro then
// redirects every downstream `Serial` reference to the wrapper for that
// translation unit only — framework headers above the include are
// untouched.
//
// Behaviour:
//   - Bytes accumulate per-line; on '\n' the line gets pushed to the
//     ring (CR is stripped). Over-long lines (>=240 chars without a
//     newline) are flushed eagerly so the ring never holds an unbounded
//     fragment.
//   - Bytes are also forwarded to the real USB-CDC ::Serial when the
//     host has space — same non-blocking guard dlog() already uses, so
//     a headless console never wedges on TX-buffer saturation.
//   - Begin() forwards to ::Serial so USB-CDC still gets initialised.
//
// Source files that need the *real* ::Serial (the wrapper's own .cpp,
// ring_log.cpp) define SERIAL_MIRROR_NO_OVERRIDE before including, or
// simply don't include this header.

#include <Arduino.h>
#include <Print.h>

class LineBufferingPrint : public Print {
public:
    using Print::write;   // keep Print's write() overloads visible
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    int    availableForWrite() override;
    void   flush() override;
    operator bool() const { return true; }
    void   begin(unsigned long baud);
};

extern LineBufferingPrint g_serial;

#ifndef SERIAL_MIRROR_NO_OVERRIDE
#define Serial g_serial
#endif
