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
    for (int i = 0; i < MAX_TOOLS; ++i) {
        _tools[i] = ToolUsage{};
        _diameters[i] = DEFAULT_DIAMETER;
        _densities[i] = DEFAULT_DENSITY;
    }
    for (int p = 0; p < PCT_SLOTS; ++p) {
        for (int i = 0; i < MAX_TOOLS; ++i) _mmAtPct[p][i] = -1.f;
    }
    _lineBuf = "";
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

    // G1 / G0 moves carry the E parameter we care about.
    if (line.startsWith("G1 ") || line.startsWith("G0 ")) {
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
