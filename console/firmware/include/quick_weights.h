#pragma once
#include <Arduino.h>
#include <vector>

// User-configurable shortcut weights for the new-spool wizard's "Full spool"
// step. Stored as a JSON array of gram values in NVS (config.h
// `NVS_NS_CONSOLE` / `NVS_KEY_QUICK_WEIGHTS`). Default [1000, 2000, 5000]
// covers typical consumer filament reels.
namespace QuickWeights {

// Read the current list from NVS. Returns the default when nothing has
// been saved. Capped at QUICK_WEIGHTS_MAX entries.
std::vector<int> get();

// Replace the list. Entries ≤ 0 are dropped; any excess beyond
// QUICK_WEIGHTS_MAX is truncated. Does not dedupe intentionally — the user
// may have "2500" twice if they want it to appear twice.
void set(const std::vector<int>& grams);

} // namespace QuickWeights
