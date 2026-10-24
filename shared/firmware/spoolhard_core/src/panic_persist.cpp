#include "spoolhard/panic_persist.h"
#include "spoolhard/ring_log.h"
#include <SPIFFS.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <esp_rom_sys.h>
#include <atomic>

namespace PanicPersist {

namespace {

constexpr const char* kPendingPath = "/crash_pending.txt";
constexpr const char* kSeqPath     = "/crash_seq.txt";
constexpr uint32_t    kFlushIntervalMs = 120 * 1000;
constexpr size_t      kMaxLines = 80;
constexpr bool        kGracefulOnly = false;  // future use

uint32_t s_lastFlush_ms = 0;
uint32_t s_lastFlushedSeq = 0;   // RingLog::headSeq() at last write
std::atomic<bool> s_paused{false};
const char* s_prev_reason = "UNKNOWN";

const char* _resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                 return "UNKNOWN";
    }
}

uint32_t _nextSeq() {
    uint32_t seq = 0;
    if (SPIFFS.exists(kSeqPath)) {
        File f = SPIFFS.open(kSeqPath, FILE_READ);
        if (f) { seq = (uint32_t)f.parseInt(); f.close(); }
    }
    seq++;
    File f = SPIFFS.open(kSeqPath, FILE_WRITE);
    if (f) { f.print(seq); f.close(); }
    return seq;
}

bool _writePending() {
    File f = SPIFFS.open(kPendingPath, FILE_WRITE);
    if (!f) return false;
    auto rows = RingLog::snapshot(0, kMaxLines);
    for (auto& e : rows) {
        f.printf("[%lu] %s\n", (unsigned long)e.millis_at, e.line.c_str());
    }
    f.close();
    return true;
}

}  // namespace

bool wasCrash(const char* reason) {
    if (!reason) return false;
    return strcmp(reason, "PANIC")    == 0 ||
           strcmp(reason, "INT_WDT")  == 0 ||
           strcmp(reason, "TASK_WDT") == 0 ||
           strcmp(reason, "WDT")      == 0 ||
           strcmp(reason, "BROWNOUT") == 0;
}

const char* begin() {
    s_prev_reason = _resetReasonStr(esp_reset_reason());
    Serial.printf("[Panic] previous reset reason: %s\n", s_prev_reason);

    if (wasCrash(s_prev_reason) && SPIFFS.exists(kPendingPath)) {
        uint32_t seq = _nextSeq();
        char path[40];
        snprintf(path, sizeof(path), "/crash_%lu.txt", (unsigned long)seq);
        if (SPIFFS.rename(kPendingPath, path)) {
            Serial.printf("[Panic] promoted pending log → %s (reason=%s)\n",
                          path, s_prev_reason);
        } else {
            // Promotion failed — at least delete the pending so we
            // don't keep promoting it on every subsequent boot.
            SPIFFS.remove(kPendingPath);
        }
    } else if (SPIFFS.exists(kPendingPath)) {
        // Clean reboot — pending log is irrelevant.
        SPIFFS.remove(kPendingPath);
    }
    return s_prev_reason;
}

void tick() {
    uint32_t now = millis();
    if (now - s_lastFlush_ms < kFlushIntervalMs) return;
    if (s_paused.load()) return;          // e.g. SPIFFS upload in progress
    s_lastFlush_ms = now;
    // Nothing new in the ring since the last write: skip the ~10 KB
    // SPIFFS rewrite (flash wear + a multi-ms blocking write).
    uint32_t head = RingLog::headSeq();
    if (head == s_lastFlushedSeq && SPIFFS.exists(kPendingPath)) return;
    // MERGE NOTE: any FS-lock gate (e.g. B10's tryLockFs()) goes right
    // here as `if (!tryLockFs()) return;` BEFORE the write, and
    // s_lastFlushedSeq is only set AFTER the write succeeded, so a failed
    // lock leaves the change unflushed and retried on the next interval.
    if (!_writePending()) return;
    s_lastFlushedSeq = head;
}

void pause()  { s_paused.store(true); }
void resume() { s_paused.store(false); }

void markGracefulExit() {
    SPIFFS.remove(kPendingPath);
}

}  // namespace PanicPersist
