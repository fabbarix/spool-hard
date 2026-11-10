// Host test for GCodeAnalyzer net-extrusion accounting. Run from the repo root:
//   g++ -std=c++17 -O2 -I console/firmware/test/host/stubs -I console/firmware/include console/firmware/src/gcode_analyzer.cpp console/firmware/test/host/test_gcode_analyzer.cpp -o /tmp/tga && /tmp/tga [plate.gcode ...]
// Extra args: Bambu plate gcode files (unzip Metadata/plate_N.gcode from a
// .gcode.3mf); prints per-tool grams to compare with slice_info used_g.
#include "gcode_analyzer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

static std::unique_ptr<GCodeAnalyzer> run(const char* g) {
    auto a = std::make_unique<GCodeAnalyzer>();
    a->reset();
    a->feed((const uint8_t*)g, strlen(g));
    a->finalise();
    return a;
}

int main(int argc, char** argv) {
    // Relative: 10 mm extruded, 0.8 retract + 0.8 unretract twice -> 10 mm net.
    auto a = run("M83\nG1 X1 E5\nG1 E-0.8\nG1 X2\nG1 E0.8\nG1 X3 E5\nG1 E-0.8\nG1 E0.8\n");
    assert(fabsf(a->tool(0).mm - 10.f) < 1e-3f);
    // Absolute with G92 resets: same net 10 mm.
    a = run("M82\nG92 E0\nG1 X1 E5\nG1 E4.2\nG1 E5\nG92 E0\nG1 X2 E5\n");
    assert(fabsf(a->tool(0).mm - 10.f) < 1e-3f);
    // Debt is per tool: T0 retracts, T1 extrudes, T0 unretracts.
    a = run("M83\nT0\nG1 E3\nG1 E-2\nT1\nG1 E4\nT0\nG1 E2\nG1 E1\n");
    assert(fabsf(a->tool(0).mm - 4.f) < 1e-3f);
    assert(fabsf(a->tool(1).mm - 4.f) < 1e-3f);
    puts("synthetic: ok");

    for (int i = 1; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        auto an = std::make_unique<GCodeAnalyzer>();
        an->reset();
        static uint8_t buf[1 << 16];
        size_t n;
        bool densSet = false;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            an->feed(buf, n);
            if (!densSet && an->slicerHeaderParsed()) {
                for (int t = 0; t < GCodeAnalyzer::MAX_TOOLS; ++t)
                    if (an->slicerSlot(t).density > 0.f) an->setDensity(t, an->slicerSlot(t).density);
                densSet = true;
            }
        }
        fclose(f);
        an->finalise();
        for (int t = 0; t < GCodeAnalyzer::MAX_TOOLS; ++t)
            if (an->tool(t).mm > 0.f)
                printf("%s T%d %.2f m %.2f g\n", argv[i], t, an->tool(t).mm / 1000.f, an->tool(t).grams);
    }
    return 0;
}
