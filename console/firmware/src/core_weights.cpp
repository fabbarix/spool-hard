#include "core_weights.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace {

// Single source of truth for load/save. Parses the NVS blob if present,
// else returns an empty JSON object. Mirrors the ScaleSecrets pattern so
// the file is short and the concurrency story is obvious — read, mutate,
// write back.
JsonDocument _loadAll() {
    JsonDocument doc;
    Preferences prefs;
    prefs.begin(NVS_NS_CORE_WEIGHTS, true);
    String blob = prefs.getString(NVS_KEY_CORE_MAP, "");
    prefs.end();
    if (blob.length()) deserializeJson(doc, blob);
    if (!doc.is<JsonObject>()) doc.to<JsonObject>();
    return doc;
}

void _saveAll(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    Preferences prefs;
    prefs.begin(NVS_NS_CORE_WEIGHTS, false);
    prefs.putString(NVS_KEY_CORE_MAP, out);
    prefs.end();
}

// Split "brand/material/<advertised>" back into components. Silent no-op if
// the key doesn't match the expected shape — callers treat it as "row was
// written by some other code" and skip.
bool _parseKey(const String& key, String& brand, String& material, int& advertised) {
    int s1 = key.indexOf('/');
    int s2 = key.indexOf('/', s1 + 1);
    if (s1 < 1 || s2 < 0) return false;
    brand    = key.substring(0, s1);
    material = key.substring(s1 + 1, s2);
    advertised = key.substring(s2 + 1).toInt();
    return true;
}

} // namespace

namespace CoreWeights {

String keyFor(const String& brand, const String& material, int advertised) {
    return brand + "/" + material + "/" + String(advertised);
}

int get(const String& brand, const String& material, int advertised) {
    if (brand.isEmpty() || material.isEmpty() || advertised <= 0) return -1;
    JsonDocument doc = _loadAll();
    String k = keyFor(brand, material, advertised);
    JsonVariantConst v = doc[k];
    if (!v.is<JsonObjectConst>()) return -1;
    return v["grams"] | -1;
}

void set(const String& brand, const String& material, int advertised, int grams) {
    if (brand.isEmpty() || material.isEmpty() || advertised <= 0) return;
    JsonDocument doc = _loadAll();
    JsonObject root = doc.as<JsonObject>();
    String k = keyFor(brand, material, advertised);
    if (grams < 0) {
        root.remove(k);
    } else {
        JsonObject e = root[k].is<JsonObject>() ? root[k].as<JsonObject>()
                                                : root[k].to<JsonObject>();
        e["grams"]      = grams;
        e["updated_ms"] = millis();
    }
    _saveAll(doc);
}

bool removeKey(const String& key) {
    JsonDocument doc = _loadAll();
    JsonObject root = doc.as<JsonObject>();
    if (!root[key].is<JsonVariantConst>()) return false;
    root.remove(key);
    _saveAll(doc);
    return true;
}

std::vector<Entry> list() {
    std::vector<Entry> out;
    JsonDocument doc = _loadAll();
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        Entry e{};
        if (!_parseKey(kv.key().c_str(), e.brand, e.material, e.advertised)) continue;
        e.grams      = kv.value()["grams"]      | -1;
        e.updated_ms = kv.value()["updated_ms"] | 0u;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace CoreWeights
