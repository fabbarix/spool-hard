#include "calibration_presets.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <algorithm>

namespace CalibrationPresets {

const int    DEFAULTS[]   = {100, 250, 500, 1000, 2000, 5000};
const size_t DEFAULTS_LEN = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);

namespace {

std::vector<int> _normalise(std::vector<int> in) {
    in.erase(std::remove_if(in.begin(), in.end(),
                            [](int g) { return g <= 0; }),
             in.end());
    std::sort(in.begin(), in.end());
    in.erase(std::unique(in.begin(), in.end()), in.end());
    if (in.size() > CAL_PRESETS_MAX) in.resize(CAL_PRESETS_MAX);
    return in;
}

std::vector<int> _readBlob() {
    std::vector<int> out;
    Preferences prefs;
    prefs.begin(NVS_NS_CONSOLE, true);
    String blob = prefs.getString(NVS_KEY_CAL_PRESETS, "");
    prefs.end();
    if (!blob.length()) return out;
    JsonDocument doc;
    if (deserializeJson(doc, blob)) return out;
    if (!doc.is<JsonArray>()) return out;
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        if (v.is<int>()) out.push_back(v.as<int>());
    }
    return out;
}

} // namespace

std::vector<int> list() {
    auto stored = _normalise(_readBlob());
    if (!stored.empty()) return stored;
    // Empty NVS → return the compiled-in defaults. We don't write them
    // back: leaving NVS empty signals "user hasn't customised yet" and
    // lets a future default change propagate without overwriting an
    // explicit empty list.
    return std::vector<int>(DEFAULTS, DEFAULTS + DEFAULTS_LEN);
}

void set(const std::vector<int>& presets) {
    auto clean = _normalise(presets);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int g : clean) arr.add(g);
    String out;
    serializeJson(doc, out);
    Preferences prefs;
    prefs.begin(NVS_NS_CONSOLE, false);
    prefs.putString(NVS_KEY_CAL_PRESETS, out);
    prefs.end();
}

} // namespace CalibrationPresets
