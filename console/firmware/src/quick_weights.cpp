#include "quick_weights.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace QuickWeights {

static const std::vector<int> _defaults = { 1000, 2000, 5000 };

std::vector<int> get() {
    Preferences prefs;
    prefs.begin(NVS_NS_CONSOLE, true);
    String blob = prefs.getString(NVS_KEY_QUICK_WEIGHTS, "");
    prefs.end();
    if (blob.isEmpty()) return _defaults;

    JsonDocument doc;
    if (deserializeJson(doc, blob)) return _defaults;
    if (!doc.is<JsonArray>())       return _defaults;

    std::vector<int> out;
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        int g = v | 0;
        if (g > 0) out.push_back(g);
        if ((int)out.size() >= QUICK_WEIGHTS_MAX) break;
    }
    if (out.empty()) return _defaults;
    return out;
}

void set(const std::vector<int>& grams) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int g : grams) {
        if (g <= 0) continue;
        arr.add(g);
        if ((int)arr.size() >= QUICK_WEIGHTS_MAX) break;
    }
    String out;
    serializeJson(doc, out);
    Preferences prefs;
    prefs.begin(NVS_NS_CONSOLE, false);
    prefs.putString(NVS_KEY_QUICK_WEIGHTS, out);
    prefs.end();
}

} // namespace QuickWeights
