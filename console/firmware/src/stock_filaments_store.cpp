#include "stock_filaments_store.h"
#include "config.h"
#include "sdcard.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include "spoolhard/psram_json_alloc.h"
#include "spoolhard/serial_mirror.h"

namespace {
uint32_t fnv1a(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}
uint32_t fnv1a(const String& s) { return fnv1a(s.c_str(), s.length()); }

// Prefer PSRAM; fall back to default heap if PSRAM is unavailable.
void* psramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
}  // namespace

void StockFilamentsStore::begin() {
    lock();  // create the mutex before any reader/reload races
    reload();
}

SemaphoreHandle_t StockFilamentsStore::lock() const {
    if (!_mtx) {
        // Created from begin() on loopTask before any reader exists.
        _mtx = xSemaphoreCreateMutex();
    }
    return _mtx;
}

// Swap in new buffers under the mutex; free the old ones after unlocking.
void StockFilamentsStore::install(char* buf, RowIdx* idx, size_t n, bool loaded) {
    SemaphoreHandle_t m = lock();
    xSemaphoreTake(m, portMAX_DELAY);
    char* oldBuf = _buf;
    RowIdx* oldIdx = _idx;
    _buf = buf;
    _idx = idx;
    _count = n;
    _loaded = loaded;
    xSemaphoreGive(m);
    free(oldBuf);
    free(oldIdx);
}

bool StockFilamentsStore::parseRow(const RowIdx& r, FilamentRecord& out) const {
    JsonDocument doc(&g_psramJsonAlloc);
    if (deserializeJson(doc, _buf + r.off, r.len)) return false;
    return out.fromJson(doc);
}

void StockFilamentsStore::reload() {
    if (!g_sd.isMounted()) {
        Serial.println("[StockFilaments] SD not mounted; stock library unavailable");
        install(nullptr, nullptr, 0, false);
        return;
    }
    File f = SD.open(FILAMENTS_PATH, FILE_READ);
    if (!f) {
        Serial.printf("[StockFilaments] %s not found — upload via web UI\n",
                      FILAMENTS_PATH);
        install(nullptr, nullptr, 0, false);
        return;
    }
    size_t sz = f.size();
    if (!sz) {  // empty file == loaded, zero rows
        f.close();
        install(nullptr, nullptr, 0, true);
        return;
    }
    char* buf = (char*)psramAlloc(sz + 1);
    if (!buf) {
        f.close();
        Serial.println("[StockFilaments] OOM allocating stock buffer");
        install(nullptr, nullptr, 0, false);
        return;
    }
    size_t got = f.read((uint8_t*)buf, sz);
    f.close();
    buf[got] = 0;

    // Upper bound on row count = number of newlines + 1.
    size_t maxRows = 1;
    for (size_t i = 0; i < got; i++) if (buf[i] == '\n') maxRows++;
    RowIdx* idx = (RowIdx*)psramAlloc(maxRows * sizeof(RowIdx));
    if (!idx) {
        free(buf);
        Serial.println("[StockFilaments] OOM allocating stock index");
        install(nullptr, nullptr, 0, false);
        return;
    }

    size_t n = 0, pos = 0;
    while (pos < got) {
        size_t end = pos;
        while (end < got && buf[end] != '\n') end++;
        size_t len = end - pos;
        if (len && buf[pos + len - 1] == '\r') len--;
        const char* line = buf + pos;
        if (len && len <= 0xFFFF && !(len >= 2 && line[0] == 'x' && line[1] == ' ')) {
            JsonDocument doc(&g_psramJsonAlloc);
            FilamentRecord rec;
            if (!deserializeJson(doc, line, len) && rec.fromJson(doc)) {
                idx[n].off = (uint32_t)pos;
                idx[n].len = (uint16_t)len;
                idx[n].id_hash = fnv1a(rec.setting_id);
                idx[n].type_hash = fnv1a(rec.filament_type);
                n++;
            }
        }
        pos = end + 1;
    }
    install(buf, idx, n, true);
    Serial.printf("[StockFilaments] Loaded %u stock filament rows (%u B text, PSRAM)\n",
                  (unsigned)n, (unsigned)got);
}

bool StockFilamentsStore::findById(const String& setting_id, FilamentRecord& out) const {
    uint32_t h = fnv1a(setting_id);
    SemaphoreHandle_t m = lock();
    xSemaphoreTake(m, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < _count; i++) {
        if (_idx[i].id_hash != h) continue;
        FilamentRecord r;
        if (parseRow(_idx[i], r) && r.setting_id == setting_id) {
            out = std::move(r);
            found = true;
            break;
        }
    }
    xSemaphoreGive(m);
    return found;
}

std::vector<FilamentRecord> StockFilamentsStore::list(
        size_t offset, size_t limit, const String& material_filter) const {
    std::vector<FilamentRecord> out;
    size_t skipped = 0, returned = 0;
    bool filter = material_filter.length() > 0;
    uint32_t fh = filter ? fnv1a(material_filter) : 0;
    SemaphoreHandle_t m = lock();
    xSemaphoreTake(m, portMAX_DELAY);
    for (size_t i = 0; i < _count; i++) {
        if (filter && _idx[i].type_hash != fh) continue;
        FilamentRecord r;
        if (!parseRow(_idx[i], r)) continue;
        if (filter && r.filament_type != material_filter) continue;
        if (skipped < offset) { skipped++; continue; }
        out.push_back(std::move(r));
        if (++returned >= limit) break;
    }
    xSemaphoreGive(m);
    return out;
}
