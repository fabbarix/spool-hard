#pragma once
#include <Arduino.h>
#include "config.h"   // defines PRODUCT_ID

// Product-signature handling for firmware and frontend (SPIFFS) uploads.
//
// Every built image carries a known byte pattern so the upload handler can
// reject an image meant for the sibling product (e.g. a scale SPIFFS bundle
// uploaded to a console, or vice-versa). The pattern is:
//
//     "SPOOLHARD-PRODUCT=<product-id>"
//
// where <product-id> is `PRODUCT_ID` from config.h. For firmware, the C++
// constant `__spoolhard_product_signature` below places the string into
// `.rodata`; `__attribute__((used))` stops LTO from stripping it. For SPIFFS,
// the frontend pre-build script writes a `product.txt` file with the same
// string into the data/ directory, so the raw filesystem image contains the
// bytes verbatim.
//
// The check is a streaming substring match: each upload chunk is fed through
// a KMP-style matcher *before* the bytes hit `Update.write()`. If the last
// chunk arrives and the signature never appeared, the Update is aborted.

#ifndef PRODUCT_ID
  #error "PRODUCT_ID must be defined in config.h (e.g. \"spoolscale\" or \"console\")"
#endif

#define SPOOLHARD_PRODUCT_SIGNATURE "SPOOLHARD-PRODUCT=" PRODUCT_ID

// `inline` (C++17) makes this safe to define in a header included from
// multiple TUs. `used` keeps the symbol alive through LTO so the bytes
// actually land in the built binary even if no C++ code reads them.
__attribute__((used))
inline const char __spoolhard_product_signature[] = SPOOLHARD_PRODUCT_SIGNATURE;

// Stream-matches a fixed pattern against a sequence of bytes arriving in
// arbitrary chunks. One matcher instance per upload session.
//
// Usage:
//   ProductSignatureMatcher m;   // knows its own pattern
//   m.reset();                   // at the first chunk
//   m.feed(data, len);           // for every chunk
//   if (!m.matched()) reject;    // at the final chunk
//
// The matcher uses a simple partial-index strategy (no KMP failure table):
// on mismatch we either restart at zero or treat the current byte as a
// potential new start. That's sufficient because the pattern has no
// non-trivial self-prefix ("SPOOLHARD-" never repeats the leading 'S'
// followed by 'P' anywhere inside itself).
class ProductSignatureMatcher {
public:
    void reset() { _matched = 0; _hit = false; }

    void feed(const uint8_t* data, size_t n) {
        if (_hit) return;
        const size_t plen = sizeof(SPOOLHARD_PRODUCT_SIGNATURE) - 1;  // drop \0
        const char*  pat  = SPOOLHARD_PRODUCT_SIGNATURE;
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = data[i];
            if (b == (uint8_t)pat[_matched]) {
                if (++_matched == plen) { _hit = true; return; }
            } else {
                _matched = (b == (uint8_t)pat[0]) ? 1 : 0;
            }
        }
    }

    bool matched() const { return _hit; }

private:
    size_t _matched = 0;
    bool   _hit     = false;
};
