#pragma once
#include <Arduino.h>
#include "../../include/spool_tag.h"

// On-device "register new spool" wizard.
//
// Multi-step LVGL flow that replaces the old "unknown tag → auto-stub +
// detail screen" shortcut. Flow:
//   1. New-tag prompt: [Create] / [Close].
//   2. From-scratch vs copy-template (skipped if the store is empty).
//   3. Template picker (lv_list + material chips).
//   4. Spool state (Full / Used / Empty) — skipped when the scale is offline.
//   5. Weight capture branch matching the chosen state.
//   6. Summary + save.
//
// Implementation lives in ui_wizard.cpp. The public API is deliberately
// tiny: everything else happens inside the wizard's own button callbacks.

// Kick off the wizard. Called from main.cpp's unified handleTagRead when the
// scanned UID isn't already in the store. `tag.parsed_*` fields seed the
// "new from scratch" defaults so the user starts with sensible hints when
// the tag URL exposed them.
//
// Re-entry is gated: if a wizard is already active, the new scan is
// logged and dropped.
void ui_wizard_start(const SpoolTag& tag);

// True while the wizard owns the LCD. Used by main.cpp to decide whether
// to route incoming tag scans to the wizard or to the spool-detail path.
bool ui_wizard_active();

// Push the latest scale reading into the wizard — only has an effect while
// a "Full" / "Used" / "Empty" step is showing its live-weight readout.
// Mirrors the existing ui_set_spool_live_weight contract.
void ui_wizard_on_weight(float grams, const char* state);
