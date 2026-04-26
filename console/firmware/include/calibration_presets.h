#pragma once
#include <Arduino.h>
#include <vector>

// Reference-weight presets shown on the console LCD's scale-calibration
// wizard. The user maintains the list from the console's web UI — these
// are the physical weights they actually own (a 3D-print user might
// have 100/250/500/1000 g; a jeweller might have 1/5/10/50 g). Storing
// them console-side rather than scale-side keeps the LCD UX self-
// contained and avoids a separate HTTP call against the scale.
//
// Storage: NVS namespace `console_cfg`, key `cal_presets`. Value is a
// JSON array of integers in grams, sorted ascending. Default seeded on
// first read if NVS is empty.
namespace CalibrationPresets {

// Default presets used when NVS hasn't been written yet. Picked to
// cover typical filament-spool weights and 1 kg/2 kg gym weights that
// double as calibration references.
extern const int DEFAULTS[];
extern const size_t DEFAULTS_LEN;

// Read the current list (sorted ascending, deduped). Returns the
// defaults if NVS is empty or the stored blob fails to parse.
std::vector<int> list();

// Replace the list. Filters non-positive values, dedupes, sorts
// ascending, caps at CAL_PRESETS_MAX, and persists. Pass empty to
// fall back to defaults on next read.
void set(const std::vector<int>& presets);

} // namespace CalibrationPresets
