#include "user_filaments_store.h"
#include "config.h"
#include "sdcard.h"
#include <SD.h>
#include <esp_random.h>
#include "spoolhard/serial_mirror.h"

// One-shot migration for stores written before the (printer,nozzle)
// variants model. Old records are flat — every (printer, nozzle) sibling
// of a logical filament was its own record, distinguished only by the
// `name` suffix " @<printer> <nozzle> nozzle". We walk the index, group
// records by parsed name prefix, fold all siblings into one record with
// `variants[]` populated, and rewrite the file. Idempotent: if every
// existing record already carries variants_json (post-migration), this
// is a no-op.
static String _parseGroupKey(const String& name) {
    int at = name.indexOf(" @");
    if (at < 0) return name;
    return name.substring(0, at);
}
static void _parseSuffixToVariant(const String& name, FilamentVariant& fv) {
    int at = name.indexOf(" @");
    if (at < 0) return;
    String suffix = name.substring(at + 2);   // "Bambu Lab H2S 0.4 nozzle"
    int n_pos = suffix.indexOf(" nozzle");
    if (n_pos < 0) return;
    int dia_start = suffix.lastIndexOf(' ', n_pos - 1);
    if (dia_start < 0) return;
    fv.nozzle_diameter = suffix.substring(dia_start + 1, n_pos).toFloat();
    int model_start = 0;
    if (suffix.startsWith("Bambu Lab ")) model_start = 10;
    String m = suffix.substring(model_start, dia_start);
    m.trim();
    if      (m == "X1 Carbon") m = "X1C";
    else if (m == "A1 mini")   m = "A1mini";
    else                       m.replace(" ", "");
    fv.printer_model = m;
}

void UserFilamentsStore::begin() {
    SdLock sdl(5000);  // recursive; also serialises the whole store
    _mounted = g_sd.isMounted();
    if (!_mounted) {
        Serial.println("[UserFilaments] SD not mounted; user filaments unavailable");
        return;
    }
    _recoverPack();
    _rebuildIndex();

    // Detect pre-variants records and migrate.
    std::map<String, std::vector<FilamentRecord>> groups;
    bool needsMigration = false;
    for (auto& kv : _idIndex) {
        FilamentRecord rec;
        if (!_readAt(kv.second, rec)) continue;
        if (rec.variants_json.isEmpty() && rec.name.indexOf(" @") >= 0) {
            needsMigration = true;
        }
        groups[_parseGroupKey(rec.name)].push_back(std::move(rec));
    }
    if (needsMigration) {
        Serial.printf("[UserFilaments] Migrating %u flat records → variants...\n",
                      (unsigned)_idIndex.size());
        // Rewrite the file: tombstone the lot, then append one merged
        // record per group.
        for (auto& kv : _idIndex) _tombstone(kv.second);
        _idIndex.clear();
        for (auto& g : groups) {
            const auto& siblings = g.second;
            if (siblings.empty()) continue;
            FilamentRecord merged = siblings.front();
            merged.name = g.first;  // strip the " @..." suffix
            // Variant from each sibling.
            std::vector<FilamentVariant> vs;
            std::vector<String> cloudIds;
            for (const auto& s : siblings) {
                FilamentVariant fv;
                _parseSuffixToVariant(s.name, fv);
                fv.nozzle_temp_print         = s.nozzle_temp_max;
                fv.nozzle_temp_initial_layer = s.nozzle_temp_min;
                vs.push_back(std::move(fv));
                if (s.cloud_setting_id.length()) cloudIds.push_back(s.cloud_setting_id);
            }
            merged.setVariants(vs);
            // Track sibling cloud ids so future pushes can clean up.
            if (!cloudIds.empty()) {
                JsonDocument idDoc;
                JsonArray arr = idDoc.to<JsonArray>();
                for (auto& c : cloudIds) arr.add(c);
                String idsJson; serializeJson(idDoc, idsJson);
                merged.cloud_variant_ids_json = idsJson;
                merged.cloud_setting_id = cloudIds.front();
            }
            size_t off;
            if (_append(merged, off)) _idIndex[merged.setting_id] = off;
        }
        Serial.printf("[UserFilaments] Migration done: %u groups\n",
                      (unsigned)_idIndex.size());
        pack();   // collapse the tombstones
    }

    // Edits tombstone + append, so the file only grows. Compact at boot once
    // dead lines dominate (migration above already packs when it ran).
    if (_deadLines >= 16 && _deadLines >= _idIndex.size()) {
        Serial.printf("[UserFilaments] Compacting (%u dead lines)\n", (unsigned)_deadLines);
        pack();
    }

    Serial.printf("[UserFilaments] Loaded %u user filament records\n",
                  (unsigned)_idIndex.size());
}

// See SpoolStore::_recoverPack — same protocol. Both files => original is
// authoritative, drop .tmp; only .tmp => promote it.
void UserFilamentsStore::_recoverPack() {
    const char* tmpPath = USER_FILAMENTS_PATH ".tmp";
    SdLock sdl(5000);
    if (!sdl) return;
    if (!SD.exists(tmpPath)) return;
    if (SD.exists(USER_FILAMENTS_PATH)) {
        SD.remove(tmpPath);
    } else if (SD.rename(tmpPath, USER_FILAMENTS_PATH)) {
        Serial.println("[UserFilaments] Recovered from interrupted pack");
    }
}

void UserFilamentsStore::_rebuildIndex() {
    _idIndex.clear();
    _deadLines = 0;
    if (!_mounted) return;

    SdLock sdl(5000);
    if (!sdl) return;
    File f = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!f) return;
    while (f.available()) {
        size_t lineStart = f.position();
        String line = f.readStringUntil('\n');
        if (line.length() == 0) break;
        if (line.startsWith("x ")) { _deadLines++; continue; }  // tombstone
        FilamentRecord rec;
        if (rec.fromLine(line)) {
            _idIndex[rec.setting_id] = lineStart;
        }
    }
    f.close();
}

bool UserFilamentsStore::_readAt(size_t offset, FilamentRecord& out) const {
    if (!_mounted) return false;
    SdLock sdl(1000);
    if (!sdl) return false;
    File f = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    String line = f.readStringUntil('\n');
    f.close();
    if (line.startsWith("x ")) return false;
    return out.fromLine(line);
}

bool UserFilamentsStore::_append(const FilamentRecord& rec, size_t& outOffset) {
    if (!_mounted) return false;
    SdLock sdl(1000);
    if (!sdl) return false;
    File f = SD.open(USER_FILAMENTS_PATH, FILE_APPEND);
    if (!f) {
        // First write — create the file.
        f = SD.open(USER_FILAMENTS_PATH, FILE_WRITE);
        if (!f) return false;
    }
    outOffset = f.size();
    String line = rec.toLine();
    line += '\n';
    size_t n = f.print(line);
    f.close();
    return n == line.length();
}

bool UserFilamentsStore::_tombstone(size_t offset) {
    if (!_mounted) return false;
    // SD.open with "r+" mode: in-place overwrite. Arduino's SD library
    // exposes this via FILE_WRITE on an already-existing file at a seekable
    // position. We just need to write 2 bytes ("x ") at the line start.
    SdLock sdl(1000);
    if (!sdl) return false;
    File f = SD.open(USER_FILAMENTS_PATH, "r+");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    f.print("x ");
    f.close();
    return true;
}

bool UserFilamentsStore::findById(const String& setting_id, FilamentRecord& out) {
    SdLock sdl(1000);
    if (!sdl) return false;
    auto it = _idIndex.find(setting_id);
    if (it == _idIndex.end()) return false;
    return _readAt(it->second, out);
}

bool UserFilamentsStore::upsert(const FilamentRecord& rec) {
    if (!_mounted || rec.setting_id.isEmpty()) return false;
    SdLock sdl(2000);   // makes tombstone+append (and the index) atomic
    if (!sdl) return false;

    auto it = _idIndex.find(rec.setting_id);
    if (it != _idIndex.end()) _tombstone(it->second);

    size_t offset;
    if (!_append(rec, offset)) return false;
    _idIndex[rec.setting_id] = offset;
    return true;
}

bool UserFilamentsStore::remove(const String& setting_id) {
    SdLock sdl(2000);
    if (!sdl) return false;
    auto it = _idIndex.find(setting_id);
    if (it == _idIndex.end()) return false;
    if (!_tombstone(it->second)) return false;
    _idIndex.erase(it);
    return true;
}

std::vector<FilamentRecord> UserFilamentsStore::list(
        size_t offset, size_t limit, const String& material_filter) {
    std::vector<FilamentRecord> out;
    SdLock sdl(2000);   // recursive; _readAt re-locks per record
    if (!sdl) return out;
    size_t skipped = 0, returned = 0;
    for (auto& kv : _idIndex) {
        FilamentRecord rec;
        if (!_readAt(kv.second, rec)) continue;
        if (material_filter.length() && rec.filament_type != material_filter) continue;
        if (skipped < offset) { skipped++; continue; }
        out.push_back(std::move(rec));
        if (++returned >= limit) break;
    }
    return out;
}

bool UserFilamentsStore::pack() {
    if (!_mounted) return false;
    SdLock sdl(10000);  // rare whole-file rewrite
    if (!sdl) return false;
    File in = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!in) return true;  // nothing to pack
    const char* tmpPath = USER_FILAMENTS_PATH ".tmp";
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) { in.close(); return false; }
    bool ok = true;
    size_t expect = 0;
    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        if (line.startsWith("x ") || line.length() == 0) continue;
        line += '\n';
        if (out.print(line) != line.length()) { ok = false; break; }
        expect += line.length();
    }
    in.close();
    out.close();
    if (ok) {
        File chk = SD.open(tmpPath, FILE_READ);
        ok = chk && chk.size() == expect;
        if (chk) chk.close();
    }
    if (!ok) {
        SD.remove(tmpPath);
        Serial.println("[UserFilaments] Pack failed, original untouched");
        return false;
    }
    // Original is removed only after .tmp is complete; _recoverPack()
    // handles a crash between remove and rename.
    SD.remove(USER_FILAMENTS_PATH);
    if (!SD.rename(tmpPath, USER_FILAMENTS_PATH)) {
        Serial.println("[UserFilaments] Pack rename failed (recovered next boot)");
        return false;
    }
    _rebuildIndex();
    Serial.printf("[UserFilaments] Packed, %u records remain\n",
                  (unsigned)_idIndex.size());
    return true;
}

String UserFilamentsStore::newLocalId() {
    // Mimic Bambu's PFUS<14-hex> shape but flag as local with the L
    // suffix — see filament_record.h conventions.
    char buf[24];
    uint32_t hi = esp_random();
    uint32_t lo = esp_random();
    snprintf(buf, sizeof(buf), "PFUL%08x%06x",
             (unsigned)hi, (unsigned)(lo & 0xffffff));
    return String(buf);
}
