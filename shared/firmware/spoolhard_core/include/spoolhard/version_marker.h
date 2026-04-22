#pragma once
#include <cstddef>
#include <cstdint>

// Streaming scanner for the firmware-version marker planted by
// `product_signature.h`: "SPOOLHARD-VERSION=<v>\x01" lives in .rodata
// somewhere in the binary. We can't use `esp_app_desc_t.version` (file
// offset 48) — the precompiled arduino-esp32 framework writes its own
// IDF build string into that slot at framework build time, so the
// project's FW_VERSION never lands there. We also can't post-process
// the .bin (esp-image SHA256 covers the whole binary), so a string
// marker scanned at upload time is the cleanest route.
//
// Two-phase state machine: first match the fixed prefix (KMP-style
// partial-index, same shape as `ProductSignatureMatcher`), then once
// the prefix is in we capture bytes verbatim until we see the 0x01
// sentinel — that's the version string.
//
// Subtlety: the parser's own kPrefix string literal also lives in the
// uploaded firmware's .rodata (right next to the real marker). The
// parser will hit it FIRST during a stream scan — followed by a NUL,
// not a real version. So in capture phase, any non-printable byte
// before the sentinel means "this wasn't the marker, keep scanning".
struct VersionMarkerParser {
    std::size_t  prefixMatched = 0;
    bool         capturing     = false;
    bool         parsed        = false;
    char         version[33]   = {0};
    std::size_t  versionLen    = 0;

    void reset() {
        prefixMatched = 0; capturing = false; parsed = false;
        version[0] = 0; versionLen = 0;
    }
    void feed(const std::uint8_t* data, std::size_t n);
};
