#include "scale_secrets.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace {

// NVS key the JSON blob lives at. Kept inside NVS_NS_SCALE alongside the
// paired-scale IP / name / legacy single-secret entries.
constexpr const char* SECRETS_KEY = "secrets_json";

JsonDocument _loadAll() {
    JsonDocument doc;
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, true);
    String blob = prefs.getString(SECRETS_KEY, "");
    prefs.end();
    if (!blob.isEmpty()) deserializeJson(doc, blob);
    if (!doc.is<JsonObject>()) doc.to<JsonObject>();
    return doc;
}

void _saveAll(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, false);
    prefs.putString(SECRETS_KEY, out);
    prefs.end();
}

} // namespace

String ScaleSecrets::get(const String& scaleName) {
    if (scaleName.isEmpty()) return "";
    JsonDocument all = _loadAll();
    return all[scaleName] | "";
}

void ScaleSecrets::set(const String& scaleName, const String& secret) {
    if (scaleName.isEmpty()) return;
    JsonDocument all = _loadAll();
    JsonObject root = all.as<JsonObject>();
    if (secret.isEmpty()) root.remove(scaleName);
    else                  root[scaleName] = secret;
    _saveAll(all);
}

String ScaleSecrets::preview(const String& scaleName) {
    String s = get(scaleName);
    if (s.length() > 4) return s.substring(0, 2) + "****" + s.substring(s.length() - 2);
    if (s.length())     return "****";
    return "";
}

bool ScaleSecrets::configured(const String& scaleName) {
    return get(scaleName).length() > 0;
}
