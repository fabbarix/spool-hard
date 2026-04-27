#include "gcode_analyzer.h"
#include <math.h>

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
    _lineBuf = "";
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
}

void GCodeAnalyzer::_processLine(const String& rawLine) {
    // Header comments (`; key = ...`) carry slicer→AMS hints we need
    // for the result packer. Parse them BEFORE the strip-comment step
    // — once we hit real G-code those don't recur.
    if (rawLine.length() && rawLine[0] == ';') {
        if (_maybeParseSlicerHeader(rawLine)) return;
        // Other comments are noise — fall through and let the strip-
        // comment step below ignore them.
    }
    // Strip a trailing comment if any (Bambu uses ';').
    int semi = rawLine.indexOf(';');
    String line = (semi >= 0) ? rawLine.substring(0, semi) : rawLine;
    line.trim();
    if (line.isEmpty()) return;

    // M82 = absolute extrusion, M83 = relative extrusion.
    if (line == "M82") { _relative = false; return; }
    if (line == "M83") { _relative = true;  return; }

    // Tool switch: T0, T1, … (and occasional T255 = unload). Bambu also uses
    // M620 S<n> for AMS slot loads; we treat it as a T-switch to keep the
    // tool bucket accurate even if the printer pre-loads via M620 before a T.
    if (line.length() >= 2 && line[0] == 'T' && isdigit(line[1])) {
        int t = atoi(line.c_str() + 1);
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
    if (line.startsWith("M73")) {
        int pPos = line.indexOf(" P");
        if (pPos >= 0) {
            int pct = atoi(line.c_str() + pPos + 2);
            if (pct >= 0 && pct < PCT_SLOTS) {
                for (int i = 0; i < MAX_TOOLS; ++i) _mmAtPct[pct][i] = _tools[i].mm;
                _pctEverSeen = true;
            }
        }
        return;
    }

    if (line.startsWith("M620 ")) {
        int s = line.indexOf('S');
        if (s > 0) {
            int t = atoi(line.c_str() + s + 1);
            if (t >= 0 && t < MAX_TOOLS) {
                if (_active != t) ++_swaps;
                _active = t;
                _seenAnyE = false;
            }
        }
        return;
    }

    // G92 E<n>: reset E axis reference. Can appear mid-file multiple times.
    if (line.startsWith("G92")) {
        int ePos = line.indexOf(" E");
        if (ePos >= 0) {
            _lastE = atof(line.c_str() + ePos + 2);
            _seenAnyE = true;
        }
        return;
    }

    // G0 / G1 (linear) and G2 / G3 (CW / CCW arc) all carry an E
    // parameter when extruding. Bambu's slicer enables arc-fitting by
    // default — runs of short G1 segments get rewritten as G2/G3
    // arcs, so dropping them silently under-counts mm by ~30 % on
    // any print with curved walls (1.72 m → 1.2 m kind of miss).
    if (line.startsWith("G1 ") || line.startsWith("G0 ") ||
        line.startsWith("G2 ") || line.startsWith("G3 ")) {
        int ePos = line.indexOf(" E");
        if (ePos < 0) return;
        float e = atof(line.c_str() + ePos + 2);
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
        if (c == '\r') continue;
        if (c == '\n') {
            if (_lineBuf.length()) {
                _processLine(_lineBuf);
                _lineBuf = "";
            }
        } else {
            _lineBuf += c;
            // Cap line length to guard against pathological input.
            if (_lineBuf.length() > 1024) _lineBuf = "";
        }
    }
}

void GCodeAnalyzer::finalise() {
    if (_lineBuf.length()) {
        _processLine(_lineBuf);
        _lineBuf = "";
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
bool GCodeAnalyzer::_maybeParseSlicerHeader(const String& rawLine) {
    struct Spec {
        const char* prefix;   // matched against rawLine.startsWith(...)
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
        if (rawLine.startsWith(s.prefix)) { match = &s; break; }
    }
    if (!match) return false;
    int eq = rawLine.indexOf('=');
    if (eq < 0) return true;   // matched key but no value — consume + ignore
    String values = rawLine.substring(eq + 1);
    values.trim();

    int idx = 0;
    int start = 0;
    int len = (int)values.length();
    for (int i = 0; i <= len && idx < MAX_TOOLS; ++i) {
        bool boundary = (i == len) || values[i] == ';' || values[i] == ',';
        if (!boundary) continue;
        String tok = values.substring(start, i);
        tok.trim();
        // Trim wrapping double-quotes — filament_settings_id-style
        // values use them; our three known keys don't, but cheap
        // safety for possible future additions.
        if (tok.length() >= 2 && tok[0] == '"' && tok[tok.length() - 1] == '"') {
            tok = tok.substring(1, tok.length() - 1);
        }
        if (tok.length()) {
            switch (match->kind) {
                case Spec::Ids:
                    _slicerSlots[idx].filament_id = tok;
                    break;
                case Spec::Colour: {
                    // Hex like "#FFDE0A" → 0xFFDE0A. Skip leading '#'.
                    const char* p = tok.c_str();
                    if (*p == '#') p++;
                    if (strlen(p) >= 6) {
                        _slicerSlots[idx].color_rgb =
                            (uint32_t)strtoul(String(p).substring(0, 6).c_str(), nullptr, 16);
                    }
                    break;
                }
                case Spec::Density: {
                    float d = tok.toFloat();
                    if (d > 0.f) {
                        _slicerSlots[idx].density = d;
                        _densities[idx] = d;   // override the family-default
                    }
                    break;
                }
            }
        }
        idx++;
        start = i + 1;
    }
    _slicerHeaderParsed = true;
    return true;
}
