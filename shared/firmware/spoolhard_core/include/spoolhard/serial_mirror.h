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
//   - Each write() is pushed to the ring as one entry (trailing CR/LF
//     stripped, >159 chars truncated with "..."). No heap allocation.
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
    // Hide Print::printf (which mallocs for >63-char output): formats into
    // a 192-byte stack buffer and truncates (ending "...\n") instead.
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    size_t printf(const __FlashStringHelper* fmt, ...);
};

extern LineBufferingPrint g_serial;

#ifndef SERIAL_MIRROR_NO_OVERRIDE
// The core already #defines Serial (HWCDCSerial with USB-CDC-on-boot);
// replace it explicitly. The wrapper reaches the real port via ::Serial.
#undef Serial
#define Serial g_serial
#endif
