#include "crash_logger.h"
#include "config.h"
#include "spoolhard/ring_log.h"
#include "sdcard.h"
#include <SD.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include "spoolhard/serial_mirror.h"

namespace CrashLogger {

namespace {

constexpr const char* kLogsDir       = "/spoolease/logs";
constexpr const char* kCrashesDir    = "/spoolease/crashes";
constexpr const char* kCurrentPath   = "/spoolease/logs/current.log";
constexpr const char* kPrevPath      = "/spoolease/logs/current.log.1";

// Roll the active log over to current.log.1 when it exceeds this. Picked
// to be larger than the in-memory ring (80 × ~160 B ≈ 13 KB) by a wide
// margin so a normal session never rolls — only long-running ones do —
// while still bounding total SD residency at ~128 KB across both files.
constexpr size_t kMaxCurrentBytes = 64 * 1024;

// Ring + spill cadence. 3 s means the worst-case loss on a hard panic is
// the ring entries from the last few seconds, which we'd lose anyway
// because SD writes inside an interrupt-context panic handler aren't
// possible from FreeRTOS. ~3 s is also infrequent enough that we don't
// thrash the SD's flash translation layer.
constexpr uint32_t kFlushIntervalMs = 3000;

// Cap saved crash logs so a runaway boot loop doesn't fill the SD card.
// Oldest (lowest seq) is dropped first.
constexpr size_t kMaxCrashFiles = 20;

bool              s_active           = false;
uint32_t          s_lastFlushedSeq   = 0;
uint32_t          s_lastFlushAtMs    = 0;
SemaphoreHandle_t s_mtx              = nullptr;

void _ensureMutex() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

const char* _resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Reasons we want to preserve the previous session's log for. POWERON
// and SW (esp_restart) are clean cases — no point spamming crashes/.
bool _isCrashReason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

// Filename pattern: "<seq>-<reason>.log". Keep it simple — we just need
// the seq number to be the stable id the API uses; reason is descriptive.
String _crashFilenameFor(uint32_t seq, const char* reason) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/%lu-%s.log",
             kCrashesDir, (unsigned long)seq, reason ? reason : "unknown");
    return String(buf);
}

// Parse "<seq>-<reason>.log" out of a directory entry. Returns true with
// seq + reason populated; false on any malformation.
bool _parseCrashName(const String& filename, uint32_t& seq, String& reason) {
    int dash = filename.indexOf('-');
    int dot  = filename.lastIndexOf('.');
    if (dash <= 0 || dot <= dash) return false;
    String seqStr = filename.substring(0, dash);
    seq = (uint32_t)seqStr.toInt();
    if (seq == 0 && seqStr != "0") return false;
    reason = filename.substring(dash + 1, dot);
    return true;
}

void _ensureDirs() {
    // Bare paths (no /sd prefix) — the SD library mounts at /sd internally
    // and SD.open strips the prefix. mkdir is idempotent so calling each
    // boot is fine.
    if (!SD.exists("/spoolease"))    SD.mkdir("/spoolease");
    if (!SD.exists(kLogsDir))        SD.mkdir(kLogsDir);
    if (!SD.exists(kCrashesDir))     SD.mkdir(kCrashesDir);
}

// Walk crashes/ and return entries ordered by seq ascending.
std::vector<CrashEntry> _listInternal() {
    std::vector<CrashEntry> out;
    File dir = SD.open(kCrashesDir);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return out;
    }
    while (File f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            String fname = String(f.name());
            // Some SD libs return the basename, others the full path —
            // strip everything before the last '/' defensively.
            int slash = fname.lastIndexOf('/');
            String base = slash >= 0 ? fname.substring(slash + 1) : fname;
            uint32_t seq = 0; String reason;
            if (_parseCrashName(base, seq, reason)) {
                CrashEntry e;
                e.seq        = seq;
                e.reason     = reason;
                e.size_bytes = (uint32_t)f.size();
                e.mtime_unix = (uint32_t)f.getLastWrite();
                out.push_back(e);
            }
        }
        f.close();
    }
    dir.close();
    std::sort(out.begin(), out.end(),
              [](const CrashEntry& a, const CrashEntry& b) { return a.seq < b.seq; });
    return out;
}

uint32_t _nextSeq() {
    auto entries = _listInternal();
    uint32_t max_seq = 0;
    for (const auto& e : entries) if (e.seq > max_seq) max_seq = e.seq;
    return max_seq + 1;
}

void _trimToLimit() {
    auto entries = _listInternal();
    if (entries.size() <= kMaxCrashFiles) return;
    size_t to_drop = entries.size() - kMaxCrashFiles;
    for (size_t i = 0; i < to_drop; ++i) {
        String p = _crashFilenameFor(entries[i].seq, entries[i].reason.c_str());
        SD.remove(p);
    }
}

// Stream-copy current.log → /spoolease/crashes/<seq>-<reason>.log,
// prepending a one-line header so the file is self-describing.
bool _archivePrevious(uint32_t seq, const char* reason) {
    File in = SD.open(kCurrentPath, FILE_READ);
    if (!in) return false;
    if (in.size() == 0) { in.close(); return false; }

    String dst = _crashFilenameFor(seq, reason);
    File out = SD.open(dst, FILE_WRITE);
    if (!out) { in.close(); return false; }

    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "=== crash captured at boot — reset_reason=%s, fw=%s ===\n",
                     reason, FW_VERSION);
    if (n > 0) out.write((const uint8_t*)hdr, (size_t)n);

    uint8_t buf[512];
    while (in.available()) {
        int r = in.read(buf, sizeof(buf));
        if (r <= 0) break;
        out.write(buf, (size_t)r);
    }
    in.close();
    out.close();
    return true;
}

// Truncate current.log + write the fresh-session header line.
void _resetCurrentLog(esp_reset_reason_t reason) {
    if (SD.exists(kCurrentPath)) SD.remove(kCurrentPath);
    File f = SD.open(kCurrentPath, FILE_WRITE);
    if (!f) return;
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "=== boot fw=%s reset=%s ===\n",
                     FW_VERSION, _resetReasonStr(reason));
    if (n > 0) f.write((const uint8_t*)hdr, (size_t)n);
    f.close();
}

// Format a ring entry for on-disk persistence. Uses millis() since boot
// because we don't trust SNTP to be up at the moment of the push — keeps
// the format unambiguous.
size_t _formatEntry(const RingLog::Entry& e, char* out, size_t out_sz) {
    int n = snprintf(out, out_sz, "[%lu] %s\n",
                     (unsigned long)e.millis_at, e.line.c_str());
    if (n < 0) return 0;
    if ((size_t)n >= out_sz) n = out_sz - 1;
    return (size_t)n;
}

void _rotateIfNeeded(File& current) {
    if (current.size() < kMaxCurrentBytes) return;
    current.close();
    if (SD.exists(kPrevPath)) SD.remove(kPrevPath);
    SD.rename(kCurrentPath, kPrevPath);
    // Reopen empty so the next write starts fresh.
    current = SD.open(kCurrentPath, FILE_WRITE);
    if (current) {
        const char* hdr = "=== rotated ===\n";
        current.write((const uint8_t*)hdr, strlen(hdr));
    }
}

}  // namespace

void begin() {
    _ensureMutex();
    if (!g_sd.isMounted()) {
        Serial.println("[CrashLog] SD not mounted — log persistence disabled");
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return;

    _ensureDirs();

    esp_reset_reason_t reason = esp_reset_reason();
    bool crash = _isCrashReason(reason);
    if (crash) {
        uint32_t seq = _nextSeq();
        if (_archivePrevious(seq, _resetReasonStr(reason))) {
            Serial.printf("[CrashLog] preserved previous session as crash #%lu (reason=%s)\n",
                          (unsigned long)seq, _resetReasonStr(reason));
            _trimToLimit();
        } else {
            Serial.println("[CrashLog] crash reset detected but no previous log to preserve");
        }
    } else if (SD.exists(kCurrentPath)) {
        // Clean reboot — rotate instead of delete so the previous
        // session stays reachable at /api/logs/previous. A wedge that
        // ends in a manual power cycle is a clean POWERON boot; without
        // this the only evidence of what went wrong is destroyed here.
        if (SD.exists(kPrevPath)) SD.remove(kPrevPath);
        SD.rename(kCurrentPath, kPrevPath);
    }

    _resetCurrentLog(reason);
    s_active         = true;
    s_lastFlushedSeq = 0;
    s_lastFlushAtMs  = millis();

    xSemaphoreGive(s_mtx);
}

void update() {
    if (!s_active) return;
    uint32_t now = millis();
    if (now - s_lastFlushAtMs < kFlushIntervalMs) return;
    s_lastFlushAtMs = now;
    flush();
}

void flush() {
    if (!s_active) return;
    if (!g_sd.isMounted()) return;   // card hot-removed; skip silently
    auto snap = RingLog::snapshot(s_lastFlushedSeq, /*limit*/ 256);
    if (snap.empty()) return;

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;

    // FILE_APPEND opens for write at end-of-file — exactly what we want
    // for a streaming log. Single open/close per flush keeps the SD's
    // FAT update cost amortised across all queued ring entries.
    File f = SD.open(kCurrentPath, FILE_APPEND);
    if (f) {
        char buf[260];
        for (auto& e : snap) {
            size_t n = _formatEntry(e, buf, sizeof(buf));
            if (n > 0) f.write((const uint8_t*)buf, n);
            s_lastFlushedSeq = e.seq;
        }
        _rotateIfNeeded(f);
        if (f) f.close();
    }

    xSemaphoreGive(s_mtx);
}

bool isAvailable() {
    return s_active && g_sd.isMounted();
}

std::vector<CrashEntry> list() {
    if (!isAvailable()) return {};
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return {};
    auto out = _listInternal();
    xSemaphoreGive(s_mtx);
    return out;
}

String pathFor(uint32_t seq) {
    if (!isAvailable()) return "";
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return "";
    String found;
    auto entries = _listInternal();
    for (const auto& e : entries) {
        if (e.seq == seq) {
            found = _crashFilenameFor(e.seq, e.reason.c_str());
            break;
        }
    }
    xSemaphoreGive(s_mtx);
    return found;
}

bool remove(uint32_t seq) {
    if (!isAvailable()) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = false;
    auto entries = _listInternal();
    for (const auto& e : entries) {
        if (e.seq == seq) {
            String p = _crashFilenameFor(e.seq, e.reason.c_str());
            ok = SD.remove(p);
            break;
        }
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

size_t removeAll() {
    if (!isAvailable()) return 0;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    size_t removed = 0;
    auto entries = _listInternal();
    for (const auto& e : entries) {
        String p = _crashFilenameFor(e.seq, e.reason.c_str());
        if (SD.remove(p)) ++removed;
    }
    xSemaphoreGive(s_mtx);
    return removed;
}

const char* currentLogPath() { return kCurrentPath; }
const char* prevLogPath()    { return kPrevPath; }

}  // namespace CrashLogger
