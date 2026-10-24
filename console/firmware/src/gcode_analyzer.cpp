#include "gcode_analyzer.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static constexpr float DEFAULT_DIAMETER = 1.75f;
static constexpr float DEFAULT_DENSITY  = 1.24f;   // PLA, reasonable default

void GCodeAnalyzer::reset() {
    _active   = 0;
    _lastE    = 0.f;
    _relative = false;
    _seenAnyE = false;
    _totalG   = 0.f;
    _totalMm  = 0.f;
    _swaps    = 0;
    _pctEverSeen = false;
    _slicerHeaderParsed = false;
    for (int i = 0; i < MAX_TOOLS; ++i) {
        _tools[i] = ToolUsage{};
        _diameters[i] = DEFAULT_DIAMETER;
        _densities[i] = DEFAULT_DENSITY;
        _slicerSlots[i] = SlicerSlot{};
    }
    for (int p = 0; p < PCT_SLOTS; ++p) {
        for (int i = 0; i < MAX_TOOLS; ++i) _mmAtPct[p][i] = -1.f;
    }
    _lineLen = 0;
    _line[0] = '\0';
}

const GCodeAnalyzer::SlicerSlot& GCodeAnalyzer::slicerSlot(int idx) const {
    static const SlicerSlot empty;
    if (idx < 0 || idx >= MAX_TOOLS) return empty;
    return _slicerSlots[idx];
}

void GCodeAnalyzer::setDiameter(int tool, float d) {
    if (tool >= 0 && tool < MAX_TOOLS && d > 0.f) _diameters[tool] = d;
}
void GCodeAnalyzer::setDensity(int tool, float d) {
    if (tool >= 0 && tool < MAX_TOOLS && d > 0.f) _densities[tool] = d;
}

void GCodeAnalyzer::_addExtrusion(float delta_mm) {
    if (delta_mm <= 0.f) return;  // retractions don't count toward usage
    if (_active < 0 || _active >= MAX_TOOLS) return;
    _tools[_active].mm += delta_mm;
    _totalMm += delta_mm;
    // Running grams update so callers polling totalGrams() during a
    // streaming feed() see a live progress number — used by the
    // dashboard's "Analysing…" progress bar. finalise() recomputes the
    // exact final value the same way; this is the running sum of the
    // same per-extrusion contribution, so the two converge to the same
    // number on the last move. Cost: one extra float multiply per
    // extrusion line (Bambu prints have tens of thousands).
    const float d   = _diameters[_active];
    const float den = _densities[_active];
    const float area = (3.14159265f / 4.f) * d * d;   // mm²
    _tools[_active].grams += delta_mm * area * den / 1000.f;
    _totalG               += delta_mm * area * den / 1000.f;
}

// Allocation-free line handler. `rawLine` is a NUL-terminated, mutable
// buffer (the analyzer's own _line); it is edited in place (comment
// strip / trim / token terminators). Semantics mirror the earlier
// String-based implementation exactly.
void GCodeAnalyzer::_processLine(char* rawLine) {
    // Header comments (`; key = ...`) carry slicer→AMS hints we need
    // for the result packer. Parse them BEFORE the strip-comment step
    // — once we hit real G-code those don't recur.
    if (rawLine[0] == ';') {
        if (_maybeParseSlicerHeader(rawLine)) return;
        // Other comments are noise — fall through and let the strip-
        // comment step below ignore them.
    }
    // Strip a trailing comment if any (Bambu uses ';').
    char* semi = strchr(rawLine, ';');
    if (semi) *semi = '\0';
    // Trim leading/trailing whitespace.
    char* line = rawLine;
    while (*line && isspace((unsigned char)*line)) ++line;
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1])) --n;
    line[n] = '\0';
    if (n == 0) return;

    // M82 = absolute extrusion, M83 = relative extrusion.
    if (strcmp(line, "M82") == 0) { _relative = false; return; }
    if (strcmp(line, "M83") == 0) { _relative = true;  return; }

    // Tool switch: T0, T1, … (and occasional T255 = unload). Bambu also uses
    // M620 S<n> for AMS slot loads; we treat it as a T-switch to keep the
    // tool bucket accurate even if the printer pre-loads via M620 before a T.
    if (n >= 2 && line[0] == 'T' && isdigit((unsigned char)line[1])) {
        int t = atoi(line + 1);
        if (t >= 0 && t < MAX_TOOLS) {
            if (_active != t) ++_swaps;
            _active = t;
            _seenAnyE = false;   // E reference resets around tool swaps
        }
        return;
    }
    // M73 P<n>: Bambu's slicer embeds these once per %-step and the printer
    // firmware uses them to drive `mc_percent`. Snapshot cumulative per-tool
    // extrusion at each marker so live-consume tracking can report exact
    // grams (not linear extrapolation) at every progress boundary.
    if (strncmp(line, "M73", 3) == 0) {
        const char* p = strstr(line, " P");
        if (p) {
            int pct = atoi(p + 2);
            if (pct >= 0 && pct < PCT_SLOTS) {
                for (int i = 0; i < MAX_TOOLS; ++i) _mmAtPct[pct][i] = _tools[i].mm;
                _pctEverSeen = true;
            }
        }
        return;
    }

    if (strncmp(line, "M620 ", 5) == 0) {
        const char* s = strchr(line, 'S');
        if (s && s > line) {
            int t = atoi(s + 1);
            if (t >= 0 && t < MAX_TOOLS) {
                if (_active != t) ++_swaps;
                _active = t;
                _seenAnyE = false;
            }
        }
        return;
    }

    // G92 E<n>: reset E axis reference. Can appear mid-file multiple times.
    if (strncmp(line, "G92", 3) == 0) {
        const char* p = strstr(line, " E");
        if (p) {
            _lastE = atof(p + 2);
            _seenAnyE = true;
        }
        return;
    }

    // G0 / G1 (linear) and G2 / G3 (CW / CCW arc) all carry an E
    // parameter when extruding. Bambu's slicer enables arc-fitting by
    // default — runs of short G1 segments get rewritten as G2/G3
    // arcs, so dropping them silently under-counts mm by ~30 % on
    // any print with curved walls (1.72 m → 1.2 m kind of miss).
    if (strncmp(line, "G1 ", 3) == 0 || strncmp(line, "G0 ", 3) == 0 ||
        strncmp(line, "G2 ", 3) == 0 || strncmp(line, "G3 ", 3) == 0) {
        const char* p = strstr(line, " E");
        if (!p) return;
        float e = atof(p + 2);
        if (_relative) {
            _addExtrusion(e);
        } else {
            if (_seenAnyE) _addExtrusion(e - _lastE);
            _lastE = e;
            _seenAnyE = true;
        }
    }
}

void GCodeAnalyzer::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        // '\r' dropped; '\0' dropped too (the old String-based buffer
        // silently ignored NULs because concat went through strlen).
        if (c == '\r' || c == '\0') continue;
        if (c == '\n') {
            if (_lineLen) {
                _line[_lineLen] = '\0';
                _processLine(_line);
                _lineLen = 0;
            }
        } else {
            _line[_lineLen++] = c;
            // Cap line length to guard against pathological input.
            if (_lineLen > LINE_CAP) _lineLen = 0;
        }
    }
}

void GCodeAnalyzer::finalise() {
    if (_lineLen) {
        _line[_lineLen] = '\0';
        _processLine(_line);
        _lineLen = 0;
    }
    // Convert mm of filament → grams:
    //   volume = length × π × (d/2)² mm³ = length × π × d²/4 mm³
    //   mass   = volume × density / 1000   (g/cm³ × mm³ → g via /1000)
    _totalG = 0.f;
    for (int i = 0; i < MAX_TOOLS; ++i) {
        float d = _diameters[i];
        float den = _densities[i];
        float area = 3.14159265f * d * d / 4.f;  // mm²
        _tools[i].grams = _tools[i].mm * area * den / 1000.f;
        _totalG += _tools[i].grams;
    }

    // Forward-fill the percent table so lookups never return -1 for any pct
    // in [first_seen..100]. Slots below the first marker are zero (nothing
    // has been extruded yet); slots past the last marker take the final
    // tool totals (anything the slicer emitted after the last M73).
    if (_pctEverSeen) {
        for (int t = 0; t < MAX_TOOLS; ++t) {
            float last = 0.f;
            for (int p = 0; p < PCT_SLOTS; ++p) {
                if (_mmAtPct[p][t] < 0.f) {
                    _mmAtPct[p][t] = last;
                } else {
                    last = _mmAtPct[p][t];
                }
            }
            // Anchor the tail to the true final value so the last print
            // segment (typically after M73 P100 was emitted, or a print
            // that stopped shy of P100) doesn't under-report.
            if (_tools[t].mm > last) _mmAtPct[PCT_SLOTS - 1][t] = _tools[t].mm;
        }
    }
}

float GCodeAnalyzer::mmAtPct(int pct, int tool) const {
    if (pct < 0) pct = 0;
    if (pct >= PCT_SLOTS) pct = PCT_SLOTS - 1;
    if (tool < 0 || tool >= MAX_TOOLS) return 0.f;
    float v = _mmAtPct[pct][tool];
    return v < 0.f ? 0.f : v;
}

float GCodeAnalyzer::gramsAtPct(int pct, int tool) const {
    if (tool < 0 || tool >= MAX_TOOLS) return 0.f;
    float mm = mmAtPct(pct, tool);
    float d = _diameters[tool];
    float den = _densities[tool];
    float area = 3.14159265f * d * d / 4.f;
    return mm * area * den / 1000.f;
}

// Parse Bambu's `; key = a;b;c[,d…]` slicer-header lines into the
// per-slot table. Returns true if `rawLine` matched a known key (so
// the caller can short-circuit the regular G-code path). Bambu is
// inconsistent with the delimiter — `;` for filament_ids /
// filament_colour, `,` for filament_density — so we accept either.
bool GCodeAnalyzer::_maybeParseSlicerHeader(char* rawLine) {
    struct Spec {
        const char* prefix;   // matched against the start of rawLine
        enum Kind { Ids, Colour, Density } kind;
    };
    static const Spec specs[] = {
        { "; filament_ids ",     Spec::Ids     },
        { "; filament_ids=",     Spec::Ids     },
        { "; filament_colour ",  Spec::Colour  },
        { "; filament_colour=",  Spec::Colour  },
        { "; filament_color ",   Spec::Colour  },   // US spelling, just in case
        { "; filament_color=",   Spec::Colour  },
        { "; filament_density ", Spec::Density },
        { "; filament_density=", Spec::Density },
    };
    const Spec* match = nullptr;
    for (const auto& s : specs) {
        if (strncmp(rawLine, s.prefix, strlen(s.prefix)) == 0) { match = &s; break; }
    }
    if (!match) return false;
    char* eq = strchr(rawLine, '=');
    if (!eq) return true;   // matched key but no value — consume + ignore
    char* values = eq + 1;
    while (*values && isspace((unsigned char)*values)) ++values;
    {
        size_t vn = strlen(values);
        while (vn && isspace((unsigned char)values[vn - 1])) --vn;
        values[vn] = '\0';
    }

    int idx = 0;
    char* tokStart = values;
    for (char* q = values; idx < MAX_TOOLS; ++q) {
        bool end = (*q == '\0');
        if (!end && *q != ';' && *q != ',') continue;
        *q = '\0';                      // terminate this token in place
        char* tok = tokStart;
        while (*tok && isspace((unsigned char)*tok)) ++tok;
        size_t tn = strlen(tok);
        while (tn && isspace((unsigned char)tok[tn - 1])) --tn;
        tok[tn] = '\0';
        // Trim wrapping double-quotes — filament_settings_id-style
        // values use them; our three known keys don't, but cheap
        // safety for possible future additions.
        if (tn >= 2 && tok[0] == '"' && tok[tn - 1] == '"') {
            tok[tn - 1] = '\0';
            ++tok;
            tn -= 2;
        }
        if (tn) {
            switch (match->kind) {
                case Spec::Ids:
                    _slicerSlots[idx].filament_id = tok;
                    break;
                case Spec::Colour: {
                    // Hex like "#FFDE0A" → 0xFFDE0A. Skip leading '#'.
                    const char* p = tok;
                    if (*p == '#') p++;
                    if (strlen(p) >= 6) {
                        char hex[7];
                        memcpy(hex, p, 6);
                        hex[6] = '\0';
                        _slicerSlots[idx].color_rgb = (uint32_t)strtoul(hex, nullptr, 16);
                    }
                    break;
                }
                case Spec::Density: {
                    float d = atof(tok);
                    if (d > 0.f) {
                        _slicerSlots[idx].density = d;
                        _densities[idx] = d;   // override the family-default
                    }
                    break;
                }
            }
        }
        idx++;
        if (end) break;
        tokStart = q + 1;
    }
    _slicerHeaderParsed = true;
    return true;
}
