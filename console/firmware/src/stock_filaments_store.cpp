#include "stock_filaments_store.h"
#include "config.h"
#include "sdcard.h"
#include <SD.h>

void StockFilamentsStore::begin() {
    reload();
}

void StockFilamentsStore::reload() {
    _rows.clear();
    _loaded = false;
    if (!g_sd.isMounted()) {
        Serial.println("[StockFilaments] SD not mounted; stock library unavailable");
        return;
    }
    File f = SD.open(FILAMENTS_PATH, FILE_READ);
    if (!f) {
        Serial.printf("[StockFilaments] %s not found — upload via web UI\n",
                      FILAMENTS_PATH);
        return;
    }
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (!line.length() || line.startsWith("x ")) continue;
        FilamentRecord rec;
        if (rec.fromLine(line)) _rows.push_back(std::move(rec));
    }
    f.close();
    _loaded = true;
    Serial.printf("[StockFilaments] Loaded %u stock filament rows\n",
                  (unsigned)_rows.size());
}

bool StockFilamentsStore::findById(const String& setting_id, FilamentRecord& out) const {
    // Linear scan — list is small (~100 rows) and lookups are rare
    // (only on spool save / push). Index-build cost would dwarf the
    // savings.
    for (auto& r : _rows) {
        if (r.setting_id == setting_id) { out = r; return true; }
    }
    return false;
}

std::vector<FilamentRecord> StockFilamentsStore::list(
        size_t offset, size_t limit, const String& material_filter) const {
    std::vector<FilamentRecord> out;
    size_t skipped = 0, returned = 0;
    for (auto& r : _rows) {
        if (material_filter.length() && r.filament_type != material_filter) continue;
        if (skipped < offset) { skipped++; continue; }
        out.push_back(r);
        if (++returned >= limit) break;
    }
    return out;
}
