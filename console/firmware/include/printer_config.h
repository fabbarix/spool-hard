#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "config.h"

// One Bambu Lab printer the console is paired with.
struct PrinterConfig {
    String name;                    // user-friendly label
    String serial;                  // MQTT username, unique key
    String access_code;             // 8-char code printed on the printer
    String ip;                      // LAN-mode IP (bonjour isn't used)
    bool   auto_restore_k    = true;   // reserved for M3
    bool   track_print_consume = true; // reserved for M3
    // Manual AMS tray → spool overrides that take precedence over automatic
    // tag_uid resolution. Stored as a JSON object keyed by "<ams_unit>:<slot_id>"
    // (e.g. "0:2"), value = spool id. Empty string means no overrides set.
    // Persisted alongside the rest of the printer config in NVS.
    String ams_overrides_json;

    void toJson(JsonDocument& doc, bool include_secret) const;
    bool fromJson(const JsonDocument& doc);

    // Look up the spool id manually mapped to (ams_unit, slot_id). Returns
    // "" if no override is set for that slot.
    String findAmsOverride(int ams_unit, int slot_id) const;
    // Set or clear (when spool_id is empty) an override for a slot. Mutates
    // `ams_overrides_json` in place; caller must save() to persist.
    void   setAmsOverride(int ams_unit, int slot_id, const String& spool_id);
};

// List of configured printers (hard cap BAMBU_MAX_PRINTERS = 5).
class PrintersConfig {
public:
    void load();
    void save() const;

    const std::vector<PrinterConfig>& list() const { return _list; }

    // Upsert by serial; returns true if new, false if updated existing.
    bool upsert(const PrinterConfig& p);
    bool remove(const String& serial);

    const PrinterConfig* find(const String& serial) const;

private:
    std::vector<PrinterConfig> _list;
};

extern PrintersConfig g_printers_cfg;
