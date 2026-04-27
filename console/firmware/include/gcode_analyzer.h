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

    // Per-slicer-slot metadata captured from Bambu's gcode header
    // comments at the top of the file. Indexed by slicer project-
    // filament index (0-based — matches the gcode's `T<n>` numbering).
    // Used by the result packer to map T<n> → physical AMS slot when
    // the .bbl manifest's `ams mapping` isn't available, and to
    // override the per-tool density with the slicer's exact value
    // for accurate gram totals on mixed-material prints.
    struct SlicerSlot {
        String   filament_id;   // "GFG00" — matches AMS tray's tray_info_idx
        uint32_t color_rgb = 0; // 0xRRGGBB; 0 means "no colour parsed"
        float    density   = 0.f; // g/cm³; 0 means "no density parsed"
    };
    const SlicerSlot& slicerSlot(int idx) const;
    bool              slicerHeaderParsed() const { return _slicerHeaderParsed; }

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

    // Slicer header metadata (one entry per project-filament slot in
    // Bambu Studio). Populated as we encounter `; filament_ids =` /
    // `; filament_colour =` / `; filament_density =` comment lines at
    // the top of the gcode. _slicerHeaderParsed flips true on the
    // first such match so callers know whether to trust the table.
    SlicerSlot _slicerSlots[MAX_TOOLS];
    bool       _slicerHeaderParsed = false;

    void _processLine(const String& line);
    void _addExtrusion(float delta_mm);
    // Try to parse a header `; key = a;b;c[;d…]` line into slicer-slot
    // metadata. Returns true if the line matched and was consumed (so
    // _processLine can skip the normal G-code branch).
    bool _maybeParseSlicerHeader(const String& rawLine);
};
