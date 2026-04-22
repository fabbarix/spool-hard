#include "spoolhard/version_marker.h"

// Stream-scan the upload for "SPOOLHARD-VERSION=<v>\x01" planted by
// product_signature.h. Phase 1 matches the literal prefix one byte at a
// time; phase 2 captures printable bytes verbatim until the 0x01 sentinel.
//
// Important subtlety: the parser's own kPrefix string literal also lives in
// the uploaded firmware's .rodata (right next to the real marker). The
// parser will hit it FIRST during a stream scan — followed by a NUL, not a
// real version. So in capture phase, any non-printable byte before the
// sentinel means "this wasn't the marker, keep scanning".
void VersionMarkerParser::feed(const std::uint8_t* data, std::size_t n) {
    if (parsed) return;
    static const char        kPrefix[]  = "SPOOLHARD-VERSION=";
    static const std::size_t kPrefixLen = sizeof(kPrefix) - 1;  // drop trailing NUL
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = data[i];
        if (!capturing) {
            if (b == (std::uint8_t)kPrefix[prefixMatched]) {
                if (++prefixMatched == kPrefixLen) {
                    capturing  = true;
                    versionLen = 0;
                }
            } else {
                prefixMatched = (b == (std::uint8_t)kPrefix[0]) ? 1 : 0;
            }
            continue;
        }
        // Capture phase.
        if (b == 0x01) {
            version[versionLen] = 0;
            parsed = true;
            return;
        }
        bool printable = (b >= 0x20 && b <= 0x7E);
        if (!printable || versionLen + 1 >= sizeof(version)) {
            // Bogus capture (NUL after the parser's own kPrefix literal,
            // or runaway length). Abandon this match attempt and resume
            // prefix scanning from the current byte.
            capturing     = false;
            versionLen    = 0;
            prefixMatched = (b == (std::uint8_t)kPrefix[0]) ? 1 : 0;
            continue;
        }
        version[versionLen++] = (char)b;
    }
}
