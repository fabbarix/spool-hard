#include "spool_tag.h"

// Shared NDEF-URL interpreter for SpoolHard-style tags. Recognises the known
// URL prefixes (info.filament3d.org/V1|V2, openprinttag) so the UI can show
// the format hint, and pulls `?m=` / `?b=` / `?c=` query parameters into
// parsed_material / parsed_brand / parsed_color_hex. Unknown URLs leave the
// fields empty; the tag is still valid, just uncharacterised.
void SpoolTag::parseUrl(const String& url, SpoolTag& out) {
    if (url.isEmpty()) return;

    if      (url.indexOf("info.filament3d.org/V1") >= 0) out.format = "SpoolHardV1";
    else if (url.indexOf("info.filament3d.org/V2") >= 0) out.format = "SpoolHardV2";
    else if (url.indexOf("openprinttag") >= 0)           out.format = "OpenPrintTag";

    int q = url.indexOf('?');
    if (q < 0) return;
    String query = url.substring(q + 1);

    int start = 0;
    while (start < (int)query.length()) {
        int amp = query.indexOf('&', start);
        if (amp < 0) amp = query.length();
        String pair = query.substring(start, amp);
        int eq = pair.indexOf('=');
        if (eq > 0) {
            String k = pair.substring(0, eq);
            String v = pair.substring(eq + 1);
            if      (k == "m" || k == "material") out.parsed_material  = v;
            else if (k == "b" || k == "brand")    out.parsed_brand     = v;
            else if (k == "c" || k == "color")    out.parsed_color_hex = v;
        }
        start = amp + 1;
    }
}
