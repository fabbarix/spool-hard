#pragma once
#include <Arduino.h>

/**
 * Streaming gcode analyzer. Feed chunks of Bambu-style gcode text with
 * feed(); finalise() returns per-tool extruded length (mm) and mass (g).
 *
 * Tracks:
 *   - Active tool (T<n>) — defaults to 0 until the first T command.
 *   - Extruder position (E axis). G92 E<x> resets to <x>. Relative extrusion
 *     (M83) is supported per-move by detecting G1 E<x> vs absolute — we use
 *     the last absolute E as a reference.
 *
 * Caller may set per-tool filament diameter (default 1.75 mm) and density
 * (default 1.24 g/cm³ ≈ PLA). Usage is per-tool so H2D dual-extruder prints
 * and AMS multi-material swaps land in separate buckets.
 */
class GCodeAnalyzer {
public:
    static constexpr int MAX_TOOLS = 16;

    struct ToolUsage {
        float mm    = 0.f;   // total extruded length
        float grams = 0.f;   // computed at finalise() from mm + diameter + density
    };

    void reset();
    void feed(const uint8_t* data, size_t len);
    void finalise();

    void setDiameter(int tool, float diameter_mm);
    void setDensity(int tool, float density_g_cm3);

    const ToolUsage& tool(int i) const { return _tools[i]; }
    int     activeTool()           const { return _active; }
    float   totalGrams()           const { return _totalG; }
    float   totalMm()              const { return _totalMm; }
    int     filamentSwaps()        const { return _swaps; }

    // Progress-indexed snapshot tables — populated from Bambu's `M73 P<n>`
    // progress hints. Index [0..100] in pct, [0..MAX_TOOLS) by tool. Valid
    // only when hasPercentTable() is true (i.e. we saw at least one M73).
    // Terminal slots beyond the last M73 are forward-filled with the final
    // tool totals so lookups at pct=100 always return the full usage.
    bool   hasPercentTable()            const { return _pctEverSeen; }
    float  mmAtPct(int pct, int tool)   const;
    float  gramsAtPct(int pct, int tool) const;

private:
    int        _active = 0;
    float      _lastE  = 0.f;      // last seen absolute E (from G1/G92)
    bool       _relative = false;  // M83 (relative extrusion mode)
    bool       _seenAnyE = false;  // false until we've seen at least one E
    ToolUsage  _tools[MAX_TOOLS];
    float      _diameters[MAX_TOOLS];
    float      _densities[MAX_TOOLS];
    float      _totalG  = 0.f;
    float      _totalMm = 0.f;
    int        _swaps   = 0;

    // Percent→cumulative-mm table, snapshotted on each `M73 P<n>` marker.
    // Unset slots hold -1 during streaming; forward-filled on finalise().
    static constexpr int PCT_SLOTS = 101;
    float      _mmAtPct[PCT_SLOTS][MAX_TOOLS];
    bool       _pctEverSeen = false;

    // Line-accumulating state: incoming chunks can break mid-line so we
    // buffer until we hit '\n'.
    String _lineBuf;

    void _processLine(const String& line);
    void _addExtrusion(float delta_mm);
};
