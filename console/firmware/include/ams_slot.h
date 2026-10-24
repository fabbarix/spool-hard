#pragma once
// Pure (no Arduino deps -- host-testable) normaliser for (ams_unit, slot_id)
// pairs as used by the ams-mapping override table and the web endpoints.
//
// Canonical addressing:
//   * real AMS units: unit 0..kAmsMaxUnits-1, slot 0..kAmsSlotsPerUnit-1
//   * external spool holder: unit 254, slot ALWAYS 0. Wire id 255 (H2 "main"
//     virtual slot) folds into 254, matching the firmware-internal keying.
// Legacy clients (the dashboard before the fix) sent slot_id = tray id (254)
// for the external card, producing the stray key "254:254".
constexpr int kAmsMaxUnits     = 4;
constexpr int kAmsSlotsPerUnit = 4;
constexpr int kAmsExternalUnit = 254;

// Returns false if the pair is out of range. On true, unit/slot are canonical.
inline bool normalizeAmsSlot(int& unit, int& slot) {
    if (unit == 254 || unit == 255) { unit = kAmsExternalUnit; slot = 0; return true; }
    return unit >= 0 && unit < kAmsMaxUnits && slot >= 0 && slot < kAmsSlotsPerUnit;
}
