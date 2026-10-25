#include "spoolhard/backup.h"
#include "spoolhard/psram_json_alloc.h"
#include "spoolhard/ring_log.h"

#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_partition.h>
#include <ctime>
#include <memory>

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

// Dump every key in `ns` into the supplied JSON array. ESP-IDF 5.x NVS
// iterator API: nvs_entry_find / nvs_entry_next take the iterator as an
// out-param and return ESP_ERR_NVS_NOT_FOUND (setting it to nullptr)
// once exhausted.
void dumpNamespace(const char* ns, JsonArray entries) {
    nvs_iterator_t it = nullptr;
    esp_err_t res = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    while (res == ESP_OK && it != nullptr) {
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
        res = nvs_entry_next(&it);  // ESP_ERR_NVS_NOT_FOUND + it=nullptr when exhausted
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
constexpr const char* kRestoreSuffix = ".restore";

// RAII bracket around one short FS operation on a mount that has an I/O
// lock hook; a no-op for mounts without one. ok() is false on timeout.
struct MountLock {
    void (*_un)() = nullptr;
    bool _ok = true;
    MountLock(bool (*l)(uint32_t), void (*u)(), uint32_t ms = 500) {
        if (l) { _ok = l(ms); if (_ok) _un = u; }
    }
    explicit MountLock(const FsMount& m, uint32_t ms = 500) : MountLock(m.lock, m.unlock, ms) {}
    ~MountLock() { if (_un) _un(); }
    MountLock(const MountLock&) = delete;
    MountLock& operator=(const MountLock&) = delete;
    bool ok() const { return _ok; }
};

const FsMount* mountForFs(const Source* src, const FS* fs) {
    if (!src) return nullptr;
    for (const auto& m : src->fs_mounts) if (m.fs == fs) return &m;
    return nullptr;
}

bool isSkipped(const String& full, const FsMount& mount) {
    for (const char* p : mount.skip_prefixes) {
        if (full.startsWith(p)) return true;
    }
    for (const char* sfx : mount.skip_suffixes) {
        if (full.endsWith(sfx)) return true;
    }
    return full.endsWith(kRestoreSuffix);
}

void walkDir(FS& fs, const String& path, const FsMount& mount, JsonArray files) {
    MountLock ml(mount, 5000);   // legacy v1 path (tests/tools only); recursive lock
    if (!ml.ok()) return;
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

        bool skip = isSkipped(full, mount);

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

void ensureParentDirs(FS& fs, const String& path) {
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
}

bool writeFile(FS& fs, const String& path, const std::vector<uint8_t>& bytes) {
    ensureParentDirs(fs, path);
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
    if (schema != 1 && schema != 2) {
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
            // v1 backups built on a PSRAM-starved device carry
            // "content": null for large files, which decodes to zero
            // bytes. Writing that would truncate e.g. the spool DB —
            // refuse anything that doesn't match its recorded size.
            if (f["size"].is<uint32_t>() && bytes.size() != f["size"].as<uint32_t>()) {
                out.errors++;
                if (out.first_error.isEmpty()) {
                    out.first_error = String("content/size mismatch for ") + path +
                                      " (corrupt backup) — not written";
                }
                continue;
            }
            const FsMount* wm = mountForFs(&src, target);
            MountLock wml(wm ? wm->lock : nullptr, wm ? wm->unlock : nullptr, 2000);
            if (wml.ok() && writeFile(*target, path, bytes)) {
                out.files_written++;
            } else {
                out.errors++;
                if (out.first_error.isEmpty()) out.first_error = String("file write failed for ") + path;
            }
        }
    }

    return out.errors == 0;
}

// ── v2: streaming tar ──────────────────────────────────────────────
namespace {

constexpr size_t kBlk = 512;
constexpr int    kSchemaTar = 2;

size_t padTo512(size_t n) { return (kBlk - (n % kBlk)) % kBlk; }

// Zero-padded octal into a NUL-terminated field of `width` bytes.
void putOctal(uint8_t* dst, size_t width, uint64_t v) {
    dst[width - 1] = 0;
    for (int i = (int)width - 2; i >= 0; --i) {
        dst[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
    }
}

uint64_t getOctal(const uint8_t* src, size_t width) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; ++i) {
        uint8_t c = src[i];
        if (c == ' ' && v == 0) continue;
        if (c < '0' || c > '7') break;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    return v;
}

unsigned headerChecksum(const uint8_t* h) {
    unsigned sum = 0;
    for (size_t i = 0; i < kBlk; ++i) {
        sum += (i >= 148 && i < 156) ? (unsigned)' ' : h[i];
    }
    return sum;
}

// POSIX ustar header. Names over 100 chars are split into prefix/name
// at a '/'; returns false if no split fits.
bool makeHeader(uint8_t* h, const String& name, size_t size, uint32_t mtime) {
    memset(h, 0, kBlk);
    const char* n   = name.c_str();
    const size_t len = name.length();
    if (len <= 100) {
        memcpy(h, n, len);
    } else {
        int cut = -1;
        for (int i = (int)len - 1; i > 0; --i) {
            if (n[i] == '/' && (len - i - 1) <= 100 && i <= 155) { cut = i; break; }
        }
        if (cut < 0) return false;
        memcpy(h + 345, n, cut);
        memcpy(h, n + cut + 1, len - cut - 1);
    }
    memcpy(h + 100, "0000644", 8);   // mode
    memcpy(h + 108, "0000000", 8);   // uid
    memcpy(h + 116, "0000000", 8);   // gid
    putOctal(h + 124, 12, size);
    putOctal(h + 136, 12, mtime);
    h[156] = '0';                    // regular file
    memcpy(h + 257, "ustar", 6);     // magic incl. NUL
    memcpy(h + 263, "00", 2);        // version
    memcpy(h + 265, "spoolhard", 9); // uname
    memcpy(h + 297, "spoolhard", 9); // gname
    putOctal(h + 148, 7, headerChecksum(h));  // 6 digits + NUL ...
    h[155] = ' ';                             // ... + space
    return true;
}

struct TarMember {
    String   name;          // archive path
    String   mem;           // in-memory payload (manifest/nvs)
    FS*      fs = nullptr;  // file payload when non-null
    bool   (*lock)(uint32_t) = nullptr;   // from the owning FsMount
    void   (*unlock)()       = nullptr;
    String   path;
    size_t   size  = 0;
    uint32_t mtime = 0;
};

// Pull-model tar generator: the AsyncWebServer response filler calls
// fill() as the socket drains. Only the open File + one 512-byte header
// are live at any time.
class TarStream {
public:
    std::vector<TarMember> members;

    size_t totalBytes() const {
        size_t t = 2 * kBlk;  // end-of-archive marker
        for (const auto& m : members) t += kBlk + m.size + padTo512(m.size);
        return t;
    }

    size_t fill(uint8_t* buf, size_t maxLen) {
        if (_aborted) return 0;   // ends the response short → client sees an error
        size_t w = 0;
        while (w < maxLen) {
            if (_cur >= members.size()) {
                size_t n = std::min(maxLen - w, _trailerLeft);
                if (!n) break;
                memset(buf + w, 0, n);
                w += n; _trailerLeft -= n;
                continue;
            }
            TarMember& m = members[_cur];
            if (_phase == Phase::Header) {
                if (_off == 0 && !makeHeader(_hdr, m.name, m.size, m.mtime)) {
                    return _abort(w, "name too long: " + m.name);
                }
                size_t n = std::min(kBlk - _off, maxLen - w);
                memcpy(buf + w, _hdr + _off, n);
                w += n; _off += n;
                if (_off == kBlk) { _phase = Phase::Data; _off = 0; }
            } else if (_phase == Phase::Data) {
                if (_off >= m.size) {
                    closeFile(m);
                    _phase = Phase::Pad; _off = 0;
                    continue;
                }
                size_t n = std::min(m.size - _off, maxLen - w);
                if (!m.fs) {
                    memcpy(buf + w, m.mem.c_str() + _off, n);
                } else {
                    // One lock per chunk read (never across the socket send).
                    MountLock ml(m.lock, m.unlock, 200);
                    if (!ml.ok()) return w ? w : (size_t)RESPONSE_TRY_AGAIN;
                    if (!_file) _file = m.fs->open(m.path, FILE_READ);
                    size_t got = _file ? _file.read(buf + w, n) : 0;
                    if (got == 0) {
                        // File vanished or shrank since the listing —
                        // never emit zero-filled content as if it were real.
                        return _abort(w, "short read on " + m.path);
                    }
                    n = got;
                }
                w += n; _off += n;
            } else {  // Pad
                size_t pad = padTo512(m.size);
                size_t n   = std::min(pad - _off, maxLen - w);
                memset(buf + w, 0, n);
                w += n; _off += n;
                if (_off == pad) { ++_cur; _phase = Phase::Header; _off = 0; }
            }
        }
        return w;
    }

    ~TarStream() {
        if (_file && _cur < members.size()) closeFile(members[_cur]);
        else if (_file) _file.close();
    }

private:
    enum class Phase { Header, Data, Pad };
    size_t  _cur = 0;
    Phase   _phase = Phase::Header;
    size_t  _off = 0;
    size_t  _trailerLeft = 2 * kBlk;
    uint8_t _hdr[kBlk];
    File    _file;
    bool    _aborted = false;

    void closeFile(TarMember& m) {
        if (!_file) return;
        MountLock ml(m.lock, m.unlock, 500);
        _file.close();
    }

    size_t _abort(size_t w, const String& why) {
        dlog("Backup", "tar stream aborted: %s", why.c_str());
        if (_cur < members.size()) closeFile(members[_cur]);
        else if (_file) _file.close();
        _aborted = true;
        return w;   // flush what's buffered; next fill() returns 0
    }
};

void collectFiles(FS& fs, const String& path, const FsMount& mount,
                  std::vector<TarMember>& out) {
    MountLock ml(mount, 5000);   // directory walk only (no file bytes); recursive
    if (!ml.ok()) return;
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }
    File f = dir.openNextFile();
    while (f) {
        String full = String(f.path() ? f.path() : f.name());
        if (full.length() == 0) full = path + "/" + f.name();
        if (!isSkipped(full, mount)) {
            if (f.isDirectory()) {
                collectFiles(fs, full, mount, out);
            } else if (mount.max_file_size == 0 || f.size() <= mount.max_file_size) {
                TarMember m;
                m.name  = String(mount.label) + (full.startsWith("/") ? "" : "/") + full;
                m.fs    = &fs;
                m.lock   = mount.lock;
                m.unlock = mount.unlock;
                m.path  = full;
                m.size  = f.size();
                m.mtime = (uint32_t)f.getLastWrite();
                out.push_back(std::move(m));
            }
        }
        File next = dir.openNextFile();
        f.close();
        f = next;
    }
    dir.close();
}

}  // namespace

void sendTarBackup(AsyncWebServerRequest* req,
                   const Source&          src,
                   const String&          product,
                   const String&          device_name,
                   const String&          fw_version,
                   const String&          filename) {
    auto st = std::make_shared<TarStream>();
    const uint32_t now = (uint32_t)time(nullptr);

    TarMember manifest;
    manifest.name  = "manifest.json";
    manifest.mtime = now;
    {
        JsonDocument d(&g_psramJsonAlloc);
        d["spoolhard_backup"] = kSchemaTar;
        d["format"]           = "tar";
        d["product"]          = product;
        d["device_name"]      = device_name;
        d["fw_version"]       = fw_version;
        d["created_at"]       = now;
        serializeJson(d, manifest.mem);
    }
    manifest.size = manifest.mem.length();
    st->members.push_back(std::move(manifest));

    TarMember nvs;
    nvs.name  = "nvs.json";
    nvs.mtime = now;
    {
        JsonDocument d(&g_psramJsonAlloc);
        JsonObject root = d.to<JsonObject>();
        for (const char* ns : src.nvs_namespaces) {
            dumpNamespace(ns, root[ns].to<JsonArray>());
        }
        serializeJson(d, nvs.mem);
    }
    nvs.size = nvs.mem.length();
    st->members.push_back(std::move(nvs));

    for (const auto& m : src.fs_mounts) {
        if (m.fs) collectFiles(*m.fs, "/", m, st->members);
    }

    const size_t total = st->totalBytes();
    dlog("Backup", "tar backup: %u members, %u bytes",
         (unsigned)st->members.size(), (unsigned)total);

    AsyncWebServerResponse* resp = req->beginResponse(
        "application/x-tar", total,
        [st](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
            return st->fill(buf, maxLen);
        });
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + filename + "\"");
    req->send(resp);
}

// ── RestoreSession ─────────────────────────────────────────────────
struct RestoreSession::Impl {
    enum class Mode  { None, Legacy, Tar, Rejected };
    enum class State { Header, Data, Pad, End };
    enum class Sink  { Skip, Mem, File };
    struct Pending { FS* fs; String path; String tmp; };

    static constexpr size_t kLegacyCap   = 256 * 1024;
    static constexpr size_t kManifestCap = 4 * 1024;
    static constexpr size_t kNvsCap      = 64 * 1024;

    const Source* src = nullptr;
    String  product;
    Mode    mode  = Mode::None;
    String  error;
    int     rejectStatus = 400;
    String  legacy;

    State   st = State::Header;
    uint8_t hdr[kBlk];
    size_t  hdrFill = 0;
    size_t  remain = 0, padRemain = 0;
    Sink    sink = Sink::Skip;
    String* memDst = nullptr;
    size_t  memCap = 0;
    bool    memIsManifest = false;
    File    out;
    Pending cur{};
    bool    sawMember   = false;
    bool    manifestOk  = false;
    String  manifest, nvs;
    std::vector<Pending> pending;
    int     filesSkipped = 0;

    ~Impl() { discardTemps(); }

    void fail(const String& why) {
        if (error.isEmpty()) error = why;
        mode = Mode::Rejected;
        discardTemps();
    }

    void discardTemps() {
        if (out) {
            const FsMount* m = mountForFs(src, cur.fs);
            MountLock ml(m ? m->lock : nullptr, m ? m->unlock : nullptr, 1000);
            out.close();
            if (cur.fs) cur.fs->remove(cur.tmp);
        }
        for (auto& p : pending) {
            const FsMount* m = mountForFs(src, p.fs);
            MountLock ml(m ? m->lock : nullptr, m ? m->unlock : nullptr, 1000);
            p.fs->remove(p.tmp);
        }
        pending.clear();
    }

    void onHeader() {
        bool zero = true;
        for (size_t i = 0; i < kBlk; ++i) if (hdr[i]) { zero = false; break; }
        if (zero) { st = State::End; return; }   // end-of-archive marker

        if (memcmp(hdr + 257, "ustar", 5) != 0 ||
            headerChecksum(hdr) != (unsigned)getOctal(hdr + 148, 8)) {
            fail("not a SpoolHard backup (bad tar header)");
            return;
        }
        String name;
        if (hdr[345]) {
            name = String((const char*)hdr + 345).substring(0, 155) + "/";
        }
        char nm[101]; memcpy(nm, hdr, 100); nm[100] = 0;
        name += nm;
        const size_t size = (size_t)getOctal(hdr + 124, 12);
        const char   type = (char)hdr[156];

        remain    = (type == '0' || type == '\0') ? size : 0;
        padRemain = (type == '0' || type == '\0') ? padTo512(size) : 0;
        if (type != '0' && type != '\0' && type != '5') {
            // Unknown member type: still skip over its data.
            remain = size; padRemain = padTo512(size);
        }
        sink = Sink::Skip;

        const bool first = !sawMember;
        sawMember = true;
        if (first && name != "manifest.json") {
            fail("not a SpoolHard backup (manifest.json must come first)");
            return;
        }
        if (name == "manifest.json") {
            sink = Sink::Mem; memDst = &manifest; memCap = kManifestCap; memIsManifest = true;
        } else if (!manifestOk) {
            fail("manifest.json missing or invalid");
            return;
        } else if (name == "nvs.json") {
            sink = Sink::Mem; memDst = &nvs; memCap = kNvsCap; memIsManifest = false;
        } else if (type == '0' || type == '\0') {
            routeFile(name);
            if (mode == Mode::Rejected) return;
        }
        if (remain == 0) memberEnd();
        st = remain ? State::Data : (padRemain ? State::Pad : State::Header);
    }

    void routeFile(const String& name) {
        int slash = name.indexOf('/');
        FS* target = nullptr;
        if (slash > 0) {
            String label = name.substring(0, slash);
            for (const auto& m : src->fs_mounts) {
                if (m.fs && label == m.label) { target = m.fs; break; }
            }
        }
        String path = slash > 0 ? name.substring(slash) : String();
        if (!target || path.length() < 2 || path.indexOf("/../") >= 0 ||
            path.endsWith("/..")) {
            filesSkipped++;
            return;
        }
        cur = Pending{target, path, path + kRestoreSuffix};
        {
            const FsMount* m = mountForFs(src, target);
            MountLock ml(m ? m->lock : nullptr, m ? m->unlock : nullptr, 1000);
            if (!ml.ok()) { fail("storage busy for " + cur.tmp); return; }
            ensureParentDirs(*target, cur.tmp);
            out = target->open(cur.tmp, FILE_WRITE);
        }
        if (!out) { fail("cannot write " + cur.tmp); return; }
        sink = Sink::File;
    }

    void write(const uint8_t* data, size_t n) {
        if (sink == Sink::Mem) {
            if (memDst->length() + n > memCap) { fail("archive member too large"); return; }
            memDst->concat((const char*)data, n);
        } else if (sink == Sink::File) {
            const FsMount* m = mountForFs(src, cur.fs);
            MountLock ml(m ? m->lock : nullptr, m ? m->unlock : nullptr, 500);
            if (!ml.ok()) { fail("storage busy writing " + cur.tmp); return; }
            if (out.write(data, n) != n) fail("write failed for " + cur.tmp);
        }
    }

    void memberEnd() {
        if (sink == Sink::Mem && memIsManifest) {
            JsonDocument d(&g_psramJsonAlloc);
            if (deserializeJson(d, manifest)) { fail("manifest.json is not valid JSON"); return; }
            if ((d["spoolhard_backup"] | 0) != kSchemaTar) {
                fail("not a SpoolHard backup (or unsupported schema)");
                return;
            }
            String why;
            if (!validate(d, product, why)) { fail(why); return; }
            manifestOk = true;
        } else if (sink == Sink::File) {
            {
                const FsMount* m = mountForFs(src, cur.fs);
                MountLock ml(m ? m->lock : nullptr, m ? m->unlock : nullptr, 1000);
                out.close();
            }
            pending.push_back(cur);
            cur = Pending{};
        }
        sink = Sink::Skip;
    }

    void feedTar(const uint8_t* data, size_t len) {
        while (len && mode == Mode::Tar) {
            switch (st) {
                case State::Header: {
                    size_t n = std::min(kBlk - hdrFill, len);
                    memcpy(hdr + hdrFill, data, n);
                    hdrFill += n; data += n; len -= n;
                    if (hdrFill == kBlk) { hdrFill = 0; onHeader(); }
                    break;
                }
                case State::Data: {
                    size_t n = std::min(remain, len);
                    write(data, n);
                    remain -= n; data += n; len -= n;
                    if (!remain && mode == Mode::Tar) {
                        memberEnd();
                        st = padRemain ? State::Pad : State::Header;
                    }
                    break;
                }
                case State::Pad: {
                    size_t n = std::min(padRemain, len);
                    padRemain -= n; data += n; len -= n;
                    if (!padRemain) st = State::Header;
                    break;
                }
                case State::End:
                    return;   // trailing zero blocks / padding
            }
        }
    }
};

RestoreSession::~RestoreSession() { delete _impl; }

void RestoreSession::begin(const Source* src, const String& expected_product) {
    delete _impl;
    _impl = new Impl();
    _impl->src     = src;
    _impl->product = expected_product;
}

void RestoreSession::reject(const String& reason, int http_status) {
    if (!_impl) return;
    _impl->fail(reason);
    _impl->rejectStatus = http_status;
}

void RestoreSession::feed(const uint8_t* data, size_t len, size_t index, size_t total) {
    Impl* s = _impl;
    if (!s || s->mode == Impl::Mode::Rejected || len == 0) return;
    if (s->mode == Impl::Mode::None) {
        if (index != 0) { s->fail("upload started mid-stream"); return; }
        if (data[0] == '{') {
            if (total > Impl::kLegacyCap) {
                s->fail("legacy JSON backup too large (>256 KB)");
                return;
            }
            s->mode = Impl::Mode::Legacy;
            if (total) s->legacy.reserve(total);
        } else {
            s->mode = Impl::Mode::Tar;
        }
    }
    if (s->mode == Impl::Mode::Legacy) {
        if (s->legacy.length() + len > Impl::kLegacyCap) {
            s->fail("legacy JSON backup too large (>256 KB)");
            return;
        }
        s->legacy.concat((const char*)data, len);
    } else {
        s->feedTar(data, len);
    }
}

int RestoreSession::finish(RestoreReport& rep, String& error) {
    rep = RestoreReport{};
    error = "";
    std::unique_ptr<Impl> s(_impl);
    _impl = nullptr;
    if (!s) { error = "no upload"; return 400; }

    if (s->mode == Impl::Mode::Rejected) { error = s->error; return s->rejectStatus; }
    if (s->mode == Impl::Mode::None)     { error = "empty upload"; return 400; }

    if (s->mode == Impl::Mode::Legacy) {
        JsonDocument doc(&g_psramJsonAlloc);
        if (deserializeJson(doc, s->legacy)) { error = "backup is not valid JSON"; return 400; }
        s->legacy = String();
        if (!validate(doc, s->product, error)) return 400;
        bool ok = applyRestore(*s->src, doc, rep);
        return ok ? 200 : 500;
    }

    // Tar: only commit if the archive arrived whole.
    if (s->st != Impl::State::End) {
        s->fail("upload incomplete (truncated archive)");
        error = s->error;
        return 400;
    }
    if (!s->manifestOk) { error = "manifest.json missing or invalid"; return 400; }

    JsonDocument nvsDoc(&g_psramJsonAlloc);
    if (s->nvs.length()) {
        JsonDocument parsed(&g_psramJsonAlloc);
        if (deserializeJson(parsed, s->nvs)) {
            s->fail("nvs.json is not valid JSON");
            error = s->error;
            return 400;
        }
        nvsDoc["nvs"] = parsed.as<JsonObjectConst>();
    }

    // Files first (rename temps over originals), then NVS.
    for (auto& p : s->pending) {
        const FsMount* pm = mountForFs(s->src, p.fs);
        MountLock ml(pm ? pm->lock : nullptr, pm ? pm->unlock : nullptr, 2000);
        if (!ml.ok()) {
            rep.errors++;
            if (rep.first_error.isEmpty()) rep.first_error = "storage busy renaming " + p.path;
            continue;
        }
        if (p.fs->exists(p.path)) p.fs->remove(p.path);
        if (p.fs->rename(p.tmp, p.path)) {
            rep.files_written++;
        } else {
            rep.errors++;
            if (rep.first_error.isEmpty()) rep.first_error = "rename failed for " + p.path;
            p.fs->remove(p.tmp);
        }
    }
    s->pending.clear();

    RestoreReport nvsRep;
    applyRestore(*s->src, nvsDoc, nvsRep);
    rep.nvs_keys_set     = nvsRep.nvs_keys_set;
    rep.nvs_keys_skipped = nvsRep.nvs_keys_skipped;
    rep.errors          += nvsRep.errors;
    if (rep.first_error.isEmpty()) rep.first_error = nvsRep.first_error;
    rep.files_skipped    = s->filesSkipped;
    dlog("Backup", "tar restore: %d files, %d nvs keys, %d skipped, %d errors",
         rep.files_written, rep.nvs_keys_set, rep.files_skipped, rep.errors);
    return rep.errors ? 500 : 200;
}

}  // namespace SpoolhardBackup
