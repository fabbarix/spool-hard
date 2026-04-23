#include "spoolhard/backup.h"

#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_partition.h>
#include <ctime>

namespace SpoolhardBackup {

// ── base64 ────────────────────────────────────────────────────────
// Tiny standalone codec — pulling mbedtls_base64 in is heavier than
// 60 lines and the ESP-IDF version's API differs slightly between
// framework versions. This is deliberately minimal; no chunking,
// caller passes the whole blob.
namespace {

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String b64encode(const uint8_t* data, size_t len) {
    String out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out += kB64[(v >> 18) & 0x3f];
        out += kB64[(v >> 12) & 0x3f];
        out += (i + 1 < len) ? kB64[(v >>  6) & 0x3f] : '=';
        out += (i + 2 < len) ? kB64[(v      ) & 0x3f] : '=';
    }
    return out;
}

bool b64decode(const char* in, size_t inLen, std::vector<uint8_t>& out) {
    static int8_t table[256];
    static bool   tableReady = false;
    if (!tableReady) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 64;  ++i) table[(int)kB64[i]] = (int8_t)i;
        table[(int)'='] = -2;  // pad sentinel
        tableReady = true;
    }
    out.clear();
    out.reserve((inLen / 4) * 3);
    int  v = 0, bits = 0;
    for (size_t i = 0; i < inLen; ++i) {
        int8_t t = table[(uint8_t)in[i]];
        if (t == -1) {
            // Permit whitespace; reject everything else.
            if (in[i] == ' ' || in[i] == '\n' || in[i] == '\r' || in[i] == '\t') continue;
            return false;
        }
        if (t == -2) break;
        v = (v << 6) | t;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((v >> bits) & 0xff));
        }
    }
    return true;
}

// ── NVS helpers ─────────────────────────────────────────────────
const char* nvsTypeName(nvs_type_t t) {
    switch (t) {
        case NVS_TYPE_U8:   return "u8";
        case NVS_TYPE_I8:   return "i8";
        case NVS_TYPE_U16:  return "u16";
        case NVS_TYPE_I16:  return "i16";
        case NVS_TYPE_U32:  return "u32";
        case NVS_TYPE_I32:  return "i32";
        case NVS_TYPE_U64:  return "u64";
        case NVS_TYPE_I64:  return "i64";
        case NVS_TYPE_STR:  return "str";
        case NVS_TYPE_BLOB: return "blob";
        default:            return "?";
    }
}

// Dump every key in `ns` into the supplied JSON array. The arduino-esp32
// framework currently ships ESP-IDF 4.4.x, which uses the older
// nvs_entry_find / nvs_entry_next API (returns iterator directly,
// nullptr when exhausted). Newer IDF (>=5.0) takes an out-param —
// don't migrate to that without bumping the framework.
void dumpNamespace(const char* ns, JsonArray entries) {
    nvs_iterator_t it = nvs_entry_find("nvs", ns, NVS_TYPE_ANY);
    while (it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);

        // Open a per-key handle for read. Cheap; NVS keeps a small cache.
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
            JsonObject e = entries.add<JsonObject>();
            e["k"] = info.key;
            e["t"] = nvsTypeName(info.type);
            switch (info.type) {
                case NVS_TYPE_U8:  { uint8_t  v = 0; nvs_get_u8 (h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_I8:  { int8_t   v = 0; nvs_get_i8 (h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_U16: { uint16_t v = 0; nvs_get_u16(h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_I16: { int16_t  v = 0; nvs_get_i16(h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_U32: { uint32_t v = 0; nvs_get_u32(h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_I32: { int32_t  v = 0; nvs_get_i32(h, info.key, &v); e["v"] = v; break; }
                case NVS_TYPE_U64: { uint64_t v = 0; nvs_get_u64(h, info.key, &v); e["v"] = (double)v; break; }
                case NVS_TYPE_I64: { int64_t  v = 0; nvs_get_i64(h, info.key, &v); e["v"] = (double)v; break; }
                case NVS_TYPE_STR: {
                    size_t sz = 0;
                    if (nvs_get_str(h, info.key, nullptr, &sz) == ESP_OK && sz > 0) {
                        std::vector<char> buf(sz);
                        if (nvs_get_str(h, info.key, buf.data(), &sz) == ESP_OK) {
                            e["v"] = (const char*)buf.data();
                        } else {
                            e["v"] = "";
                        }
                    } else {
                        e["v"] = "";
                    }
                    break;
                }
                case NVS_TYPE_BLOB: {
                    size_t sz = 0;
                    if (nvs_get_blob(h, info.key, nullptr, &sz) == ESP_OK && sz > 0) {
                        std::vector<uint8_t> buf(sz);
                        if (nvs_get_blob(h, info.key, buf.data(), &sz) == ESP_OK) {
                            e["v"] = b64encode(buf.data(), buf.size());
                        }
                    }
                    break;
                }
                default: break;
            }
            nvs_close(h);
        }
        it = nvs_entry_next(it);  // returns nullptr when exhausted
    }
    // nvs_release_iterator on nullptr is a documented no-op.
    nvs_release_iterator(it);
}

// Apply a single {k,t,v} entry into the open Preferences handle.
// Returns true on success.
bool applyNvsEntry(Preferences& prefs, JsonObjectConst e) {
    const char* k = e["k"] | "";
    const char* t = e["t"] | "";
    if (!*k || !*t) return false;
    JsonVariantConst v = e["v"];
    if (!strcmp(t, "u8"))  return prefs.putUChar (k, (uint8_t) (v | 0u))    == sizeof(uint8_t);
    if (!strcmp(t, "i8"))  return prefs.putChar  (k, (int8_t)  (v | 0))     == sizeof(int8_t);
    if (!strcmp(t, "u16")) return prefs.putUShort(k, (uint16_t)(v | 0u))    == sizeof(uint16_t);
    if (!strcmp(t, "i16")) return prefs.putShort (k, (int16_t) (v | 0))     == sizeof(int16_t);
    if (!strcmp(t, "u32")) return prefs.putUInt  (k, (uint32_t)(v | 0u))    == sizeof(uint32_t);
    if (!strcmp(t, "i32")) return prefs.putInt   (k, (int32_t) (v | 0))     == sizeof(int32_t);
    if (!strcmp(t, "u64")) return prefs.putULong64(k, (uint64_t)(v.as<double>())) == sizeof(uint64_t);
    if (!strcmp(t, "i64")) return prefs.putLong64 (k, (int64_t) (v.as<double>())) == sizeof(int64_t);
    if (!strcmp(t, "str")) {
        const char* s = v.as<const char*>();
        if (!s) s = "";
        return prefs.putString(k, s) == strlen(s);
    }
    if (!strcmp(t, "blob")) {
        const char* s = v.as<const char*>();
        if (!s) return false;
        std::vector<uint8_t> bytes;
        if (!b64decode(s, strlen(s), bytes)) return false;
        return prefs.putBytes(k, bytes.data(), bytes.size()) == bytes.size();
    }
    return false;
}

// ── FS helpers ──────────────────────────────────────────────────
void walkDir(FS& fs, const String& path, const FsMount& mount, JsonArray files) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        // path was a file (or didn't exist) — caller handles file case.
        if (dir) dir.close();
        return;
    }
    File f = dir.openNextFile();
    while (f) {
        String full = String(f.path() ? f.path() : f.name());
        if (full.length() == 0) full = path + "/" + f.name();

        bool skip = false;
        for (const char* p : mount.skip_prefixes) {
            if (full.startsWith(p)) { skip = true; break; }
        }

        if (f.isDirectory()) {
            if (!skip) walkDir(fs, full, mount, files);
        } else if (!skip) {
            size_t sz = f.size();
            if (mount.max_file_size == 0 || sz <= mount.max_file_size) {
                std::vector<uint8_t> buf(sz);
                size_t got = f.read(buf.data(), sz);
                buf.resize(got);
                JsonObject row = files.add<JsonObject>();
                row["fs"]       = mount.label;
                row["path"]     = full;
                row["size"]     = (uint32_t)got;
                row["encoding"] = "base64";
                row["content"]  = b64encode(buf.data(), buf.size());
            }
        }
        File next = dir.openNextFile();
        f.close();
        f = next;
    }
    dir.close();
}

bool writeFile(FS& fs, const String& path, const std::vector<uint8_t>& bytes) {
    // Make sure parent dirs exist. Skip "/", they should already.
    int slash = path.lastIndexOf('/');
    if (slash > 0) {
        String parent = path.substring(0, slash);
        if (!fs.exists(parent)) {
            // mkdir is shallow; do iterative parents. Most stores live
            // at the FS root anyway so this is rarely entered.
            int from = 1;
            int p = parent.indexOf('/', from);
            while (p > 0) {
                String mid = parent.substring(0, p);
                if (!fs.exists(mid)) fs.mkdir(mid);
                from = p + 1;
                p = parent.indexOf('/', from);
            }
            fs.mkdir(parent);
        }
    }
    File out = fs.open(path, FILE_WRITE);
    if (!out) return false;
    size_t n = bytes.empty() ? 0 : out.write(bytes.data(), bytes.size());
    out.close();
    return n == bytes.size();
}

}  // namespace

// ── Public API ─────────────────────────────────────────────────────
bool buildBackup(const Source& src,
                 const String& product,
                 const String& device_name,
                 const String& fw_version,
                 JsonDocument& doc,
                 String*       errors_out) {
    doc.clear();
    doc["spoolhard_backup"] = 1;
    doc["product"]          = product;
    doc["device_name"]      = device_name;
    doc["fw_version"]       = fw_version;
    doc["created_at"]       = (uint32_t)time(nullptr);

    JsonObject nvs = doc["nvs"].to<JsonObject>();
    for (const char* ns : src.nvs_namespaces) {
        JsonArray entries = nvs[ns].to<JsonArray>();
        dumpNamespace(ns, entries);
    }

    JsonArray files = doc["files"].to<JsonArray>();
    String   localErrors;
    for (const auto& m : src.fs_mounts) {
        if (!m.fs) continue;
        // Walk recursively from the FS root.
        walkDir(*m.fs, "/", m, files);
    }
    if (errors_out && localErrors.length()) *errors_out = localErrors;
    return true;
}

bool validate(const JsonDocument& doc,
              const String&       expected_product,
              String&             reason) {
    reason = "";
    int schema = doc["spoolhard_backup"] | 0;
    if (schema != 1) {
        reason = "not a SpoolHard backup (or unsupported schema)";
        return false;
    }
    String product = doc["product"] | "";
    if (product != expected_product) {
        reason = String("wrong product (expected ") + expected_product +
                 ", got " + (product.length() ? product : String("?")) + ")";
        return false;
    }
    return true;
}

bool applyRestore(const Source&       src,
                  const JsonDocument& doc,
                  RestoreReport&      out) {
    out = RestoreReport{};

    // NVS — only restore namespaces the product explicitly owns.
    JsonObjectConst nvs = doc["nvs"].as<JsonObjectConst>();
    if (!nvs.isNull()) {
        for (JsonPairConst kv : nvs) {
            const char* ns = kv.key().c_str();
            bool owned = false;
            for (const char* known : src.nvs_namespaces) {
                if (!strcmp(ns, known)) { owned = true; break; }
            }
            if (!owned) {
                out.nvs_keys_skipped += kv.value().as<JsonArrayConst>().size();
                continue;
            }
            Preferences prefs;
            if (!prefs.begin(ns, false)) {
                out.errors++;
                if (out.first_error.isEmpty()) out.first_error = String("nvs.begin failed for ") + ns;
                continue;
            }
            for (JsonVariantConst e : kv.value().as<JsonArrayConst>()) {
                if (applyNvsEntry(prefs, e.as<JsonObjectConst>())) {
                    out.nvs_keys_set++;
                } else {
                    out.errors++;
                    if (out.first_error.isEmpty()) {
                        out.first_error = String("nvs key apply failed: ") + ns + "/" +
                                          (e["k"] | "?");
                    }
                }
            }
            prefs.end();
        }
    }

    // Files — restore into the matching FS by label.
    JsonArrayConst files = doc["files"].as<JsonArrayConst>();
    if (!files.isNull()) {
        for (JsonVariantConst v : files) {
            JsonObjectConst f = v.as<JsonObjectConst>();
            const char* label = f["fs"]   | "";
            const char* path  = f["path"] | "";
            const char* enc   = f["encoding"] | "";
            if (!*label || !*path || !*enc) { out.errors++; continue; }

            FS* target = nullptr;
            for (const auto& m : src.fs_mounts) {
                if (m.fs && !strcmp(m.label, label)) { target = m.fs; break; }
            }
            if (!target) {
                out.files_skipped++;
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!strcmp(enc, "base64")) {
                const char* content = f["content"] | "";
                if (!b64decode(content, strlen(content), bytes)) {
                    out.errors++;
                    if (out.first_error.isEmpty()) out.first_error = String("base64 decode failed for ") + path;
                    continue;
                }
            } else {
                out.errors++;
                if (out.first_error.isEmpty()) out.first_error = String("unknown encoding: ") + enc;
                continue;
            }
            if (writeFile(*target, path, bytes)) {
                out.files_written++;
            } else {
                out.errors++;
                if (out.first_error.isEmpty()) out.first_error = String("file write failed for ") + path;
            }
        }
    }

    return out.errors == 0;
}

}  // namespace SpoolhardBackup
