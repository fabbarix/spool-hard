#include "printer_config.h"
#include <Preferences.h>

PrintersConfig g_printers_cfg;

void PrinterConfig::toJson(JsonDocument& doc, bool include_secret) const {
    doc["name"]    = name;
    doc["serial"]  = serial;
    doc["ip"]      = ip;
    doc["auto_restore_k"]      = auto_restore_k;
    doc["track_print_consume"] = track_print_consume;
    // Serialize the manual-override table as a nested object so the API
    // consumer (frontend / NVS blob) can read and edit it directly.
    if (ams_overrides_json.length()) {
        JsonDocument inner;
        if (!deserializeJson(inner, ams_overrides_json)) {
            doc["ams_overrides"] = inner;
        }
    }
    if (include_secret) {
        doc["access_code"] = access_code;
    } else {
        // Mask to hint at length without leaking the secret via the API.
        String masked = access_code.length() > 4
            ? access_code.substring(0, 2) + "****" + access_code.substring(access_code.length() - 2)
            : "****";
        doc["access_code_preview"] = masked;
    }
}

bool PrinterConfig::fromJson(const JsonDocument& doc) {
    name        = doc["name"]        | "";
    serial      = doc["serial"]      | "";
    ip          = doc["ip"]          | "";
    if (doc["access_code"].is<const char*>()) {
        access_code = doc["access_code"].as<String>();
    }
    if (doc["auto_restore_k"].is<bool>())      auto_restore_k      = doc["auto_restore_k"];
    if (doc["track_print_consume"].is<bool>()) track_print_consume = doc["track_print_consume"];
    if (doc["ams_overrides"].is<JsonObjectConst>()) {
        ams_overrides_json = "";
        serializeJson(doc["ams_overrides"], ams_overrides_json);
    }
    return serial.length() > 0 && access_code.length() > 0;
}

String PrinterConfig::findAmsOverride(int ams_unit, int slot_id) const {
    if (ams_overrides_json.isEmpty()) return "";
    JsonDocument d;
    if (deserializeJson(d, ams_overrides_json)) return "";
    char key[16];
    snprintf(key, sizeof(key), "%d:%d", ams_unit, slot_id);
    return d[key].as<String>();
}

void PrinterConfig::setAmsOverride(int ams_unit, int slot_id, const String& spool_id) {
    JsonDocument d;
    if (ams_overrides_json.length()) {
        if (deserializeJson(d, ams_overrides_json)) d.clear();
    }
    if (!d.is<JsonObject>()) d.to<JsonObject>();
    char key[16];
    snprintf(key, sizeof(key), "%d:%d", ams_unit, slot_id);
    if (spool_id.isEmpty()) d.remove(key);
    else                    d[key] = spool_id;
    // Empty object → clear the field entirely so we don't waste NVS on "{}".
    if (d.as<JsonObjectConst>().size() == 0) {
        ams_overrides_json = "";
        return;
    }
    ams_overrides_json = "";
    serializeJson(d, ams_overrides_json);
}

void PrintersConfig::load() {
    _list.clear();
    Preferences prefs;
    prefs.begin(NVS_NS_PRINTERS, true);
    String blob = prefs.getString(NVS_KEY_PRINTERS_LIST, "");
    prefs.end();
    if (blob.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, blob)) {
        Serial.println("[Printers] NVS blob invalid, starting empty");
        return;
    }
    for (JsonVariant v : doc.as<JsonArray>()) {
        PrinterConfig p;
        JsonDocument d;
        d.set(v);
        if (p.fromJson(d)) _list.push_back(std::move(p));
    }
    Serial.printf("[Printers] Loaded %u printer(s) from NVS\n", (unsigned)_list.size());
}

void PrintersConfig::save() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& p : _list) {
        JsonDocument inner;
        p.toJson(inner, /*include_secret*/ true);
        arr.add(inner);
    }
    String blob;
    serializeJson(doc, blob);

    Preferences prefs;
    prefs.begin(NVS_NS_PRINTERS, false);
    prefs.putString(NVS_KEY_PRINTERS_LIST, blob);
    prefs.end();
}

bool PrintersConfig::upsert(const PrinterConfig& p) {
    for (auto& existing : _list) {
        if (existing.serial == p.serial) { existing = p; save(); return false; }
    }
    if (_list.size() >= BAMBU_MAX_PRINTERS) return false;
    _list.push_back(p);
    save();
    return true;
}

bool PrintersConfig::remove(const String& serial) {
    for (auto it = _list.begin(); it != _list.end(); ++it) {
        if (it->serial == serial) {
            _list.erase(it);
            save();
            return true;
        }
    }
    return false;
}

const PrinterConfig* PrintersConfig::find(const String& serial) const {
    for (const auto& p : _list) if (p.serial == serial) return &p;
    return nullptr;
}
