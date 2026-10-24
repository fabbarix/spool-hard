// Host test for normalizeAmsSlot. Run from the repo root:
//   g++ -std=c++17 -I console/firmware/include console/firmware/test/host/test_ams_slot.cpp -o /tmp/t && /tmp/t
#include "ams_slot.h"
#include <cassert>
#include <cstdio>
static bool n(int u, int s, int eu, int es, bool ok) {
    bool r = normalizeAmsSlot(u, s);
    return r == ok && (!ok || (u == eu && s == es));
}
int main() {
    assert(n(254, 254, 254, 0, true));   // legacy dashboard payload
    assert(n(254, 0,   254, 0, true));
    assert(n(255, 255, 254, 0, true));
    assert(n(255, 0,   254, 0, true));
    assert(n(0, 0, 0, 0, true));
    assert(n(3, 3, 3, 3, true));
    assert(n(4, 0, 0, 0, false));
    assert(n(0, 4, 0, 0, false));
    assert(n(-1, 0, 0, 0, false));
    assert(n(1, -1, 0, 0, false));
    assert(n(100, 0, 0, 0, false));
    puts("ok");
}
