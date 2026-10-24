#include "spoolhard/spiffs_upload.h"
#include "spoolhard/deferred_reboot.h"
#include <SPIFFS.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <string.h>

namespace SpiffsUpload {
namespace {

constexpr uint32_t kStallMs = 20000;
constexpr size_t   kSector  = 4096;

SemaphoreHandle_t fsSem() {
    // Binary semaphore (no owner): taken by begin() on the AsyncTCP task,
    // given back by the finish task. Function-local static init is
    // thread-safe.
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = [] {
        SemaphoreHandle_t s = xSemaphoreCreateBinaryStatic(&buf);
        xSemaphoreGive(s);
        return s;
    }();
    return h;
}

volatile Phase s_phase = Phase::Idle;
const void*    s_owner = nullptr;      // active upload
const void*    s_lastOwner = nullptr;  // compare-only, for result lookup
char           s_err[96] = "";
bool           s_failed = false;
size_t         s_written = 0;
size_t         s_imageSize = 0;
char           s_expected[65] = "";
mbedtls_sha256_context s_sha;
TimerHandle_t  s_timer = nullptr;
bool           s_reboot = false;

void latch(const char* why) {
    if (!s_failed) {
        s_failed = true;
        strncpy(s_err, why, sizeof(s_err) - 1);
        s_err[sizeof(s_err) - 1] = 0;
    }
}

String hexOf(const uint8_t* d) {
    static const char* hx = "0123456789abcdef";
    String s; s.reserve(64);
    for (int i = 0; i < 32; ++i) { s += hx[d[i] >> 4]; s += hx[d[i] & 15]; }
    return s;
}

const esp_partition_t* part() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

bool remountAndWalk(const char** why) {
    if (!SPIFFS.begin(false)) { *why = "remount failed"; return false; }
    File root = SPIFFS.open("/");
    if (!root) { *why = "root open failed"; return false; }
    int files = 0;
    uint8_t buf[256];
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        size_t want = f.size(), got = 0;
        while (got < want) {
            int n = f.read(buf, sizeof(buf));
            if (n <= 0) break;
            got += n;
        }
        Serial.printf("[SpiffsUp] verify %s %u/%u\n", f.name(), (unsigned)got, (unsigned)want);
        f.close();
        if (got != want) { *why = "file read short after remount"; return false; }
        files++;
    }
    if (files == 0) { *why = "no files after remount"; return false; }
    return true;
}

// Everything after the last byte arrived. Returns true iff verified.
// Always leaves SPIFFS mounted (formatted on failure). Caller releases lock.
bool completeAndVerify(const char* clientSha) {
    const char* why = nullptr;
    uint8_t digest[32];
    mbedtls_sha256_finish(&s_sha, digest);
    mbedtls_sha256_free(&s_sha);
    bool ok = !s_failed;

    if (ok) {
        if (!Update.end(true)) { latch("Update.end failed"); ok = false; }
    } else {
        Update.abort();
    }
    if (ok && s_written == 0) { latch("empty upload"); ok = false; }
    if (ok) {
        String got = hexOf(digest);
        const char* exp = (clientSha && *clientSha) ? clientSha : s_expected;
        if (*exp && !got.equalsIgnoreCase(exp)) {
            latch("sha256 mismatch with client-supplied value"); ok = false;
        }
        Serial.printf("[SpiffsUp] %u bytes sha256=%s\n", (unsigned)s_written, got.c_str());
    }
    const esp_partition_t* p = part();
    if (ok && !p) { latch("no spiffs partition"); ok = false; }
    if (ok && s_written > p->size) { latch("image larger than partition"); ok = false; }
    if (ok) {
        // Erase whatever the image didn't cover so stale pages from the
        // previous FS can't be mistaken for data.
        size_t from = (s_written + kSector - 1) & ~(kSector - 1);
        if (from < p->size &&
            esp_partition_erase_range(p, from, p->size - from) != ESP_OK) {
            latch("tail erase failed"); ok = false;
        }
    }
    if (ok) {
        // Partition readback hash (512 B chunks: tight DRAM on the scale).
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        uint8_t buf[512];
        size_t off = 0;
        while (off < s_written) {
            size_t n = s_written - off < sizeof(buf) ? s_written - off : sizeof(buf);
            if (esp_partition_read(p, off, buf, n) != ESP_OK) { latch("partition read failed"); ok = false; break; }
            mbedtls_sha256_update(&c, buf, n);
            off += n;
            if ((off & 0xFFFF) == 0) vTaskDelay(1);
        }
        uint8_t rb[32];
        mbedtls_sha256_finish(&c, rb);
        mbedtls_sha256_free(&c);
        if (ok && memcmp(rb, digest, 32) != 0) { latch("flash readback mismatch"); ok = false; }
    }
    if (ok && !remountAndWalk(&why)) { latch(why); ok = false; }

    if (!ok) {
        Serial.printf("[SpiffsUp] FAILED (%s) — formatting SPIFFS\n", s_err);
        SPIFFS.end();
        SPIFFS.format();
        SPIFFS.begin(true);
    }
    return ok;
}

void release(bool ok) {
    s_owner = nullptr;
    s_phase = ok ? Phase::Ok : Phase::Failed;
    xSemaphoreGive(fsSem());
}

void finishTask(void*) {
    bool ok = completeAndVerify(nullptr);
    bool reboot = s_reboot;
    release(ok);
    Serial.printf("[SpiffsUp] %s\n", ok ? "verified OK" : "failed");
    if (ok && reboot) spoolhardDeferredReboot(2500);  // let clients poll the result
    vTaskDelete(nullptr);
}

void abandonTask(void*) {
    Serial.println("[SpiffsUp] upload stalled — abandoning, formatting SPIFFS");
    latch("upload stalled");
    completeAndVerify(nullptr);
    release(false);
    vTaskDelete(nullptr);
}

void spawn(TaskFunction_t fn, const char* name) {
    s_phase = Phase::Verifying;
    if (xTaskCreate(fn, name, 6144, nullptr, 1, nullptr) != pdPASS) {
        // OOM: do the work inline rather than leave the FS unmounted.
        Serial.println("[SpiffsUp] task create failed, running inline");
        bool ok = completeAndVerify(nullptr);
        bool reboot = s_reboot;
        release(ok);
        if (ok && reboot) spoolhardDeferredReboot(2500);
    }
}

void onStall(TimerHandle_t) {
    if (s_phase == Phase::Writing) spawn(abandonTask, "spiffs_abn");
}

}  // namespace

size_t partitionSize() {
    const esp_partition_t* p = part();
    return p ? p->size : 0;
}

bool tryLockFs() { return xSemaphoreTake(fsSem(), 0) == pdTRUE; }
void unlockFs()  { xSemaphoreGive(fsSem()); }

const char* begin(const void* owner, size_t imageSize) {
    const esp_partition_t* p = part();
    if (!p) return "no spiffs partition";
    size_t sz = imageSize ? imageSize : p->size;
    if (sz > p->size) return "image larger than frontend partition";
    if (xSemaphoreTake(fsSem(), pdMS_TO_TICKS(2000)) != pdTRUE)
        return "frontend upload already in progress";
    s_failed = false; s_err[0] = 0; s_written = 0; s_imageSize = sz;
    s_expected[0] = 0; s_reboot = false;
    SPIFFS.end();                       // never raw-write a mounted FS
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    if (!Update.begin(sz, U_SPIFFS)) {
        Serial.printf("[SpiffsUp] Update.begin failed: %s\n", Update.errorString());
        mbedtls_sha256_free(&s_sha);
        SPIFFS.begin(true);
        xSemaphoreGive(fsSem());
        return "Update.begin failed";
    }
    s_owner = owner;
    s_lastOwner = owner;
    s_phase = Phase::Writing;
    if (!s_timer) s_timer = xTimerCreate("spiffs_wd", pdMS_TO_TICKS(kStallMs), pdFALSE, nullptr, onStall);
    if (s_timer) xTimerReset(s_timer, 0);
    return nullptr;
}

bool owns(const void* owner) { return owner && s_owner == owner && s_phase == Phase::Writing; }

void setExpectedSha256(const void* owner, const char* hex) {
    if (!owns(owner) || !hex) return;
    strncpy(s_expected, hex, 64);
    s_expected[64] = 0;
}

bool write(const void* owner, const uint8_t* data, size_t len) {
    if (!owns(owner) || s_failed) return false;
    if (s_written + len > s_imageSize) { latch("image larger than partition"); return false; }
    size_t n = Update.write(const_cast<uint8_t*>(data), len);
    if (n != len) {
        Serial.printf("[SpiffsUp] short write %u/%u: %s\n", (unsigned)n, (unsigned)len, Update.errorString());
        latch("short write to flash");
        return false;
    }
    mbedtls_sha256_update(&s_sha, data, len);
    s_written += len;
    if (s_timer) xTimerReset(s_timer, 0);
    return true;
}

void fail(const void* owner, const char* reason) {
    if (owns(owner)) latch(reason);
}

void finish(const void* owner, bool reboot) {
    if (!owns(owner)) return;
    if (s_timer) xTimerStop(s_timer, 0);
    s_reboot = reboot;
    spawn(finishTask, "spiffs_fin");
}

bool finishBlocking(const char* expectedSha256Hex) {
    if (s_timer) xTimerStop(s_timer, 0);
    s_phase = Phase::Verifying;
    bool ok = completeAndVerify(expectedSha256Hex);
    release(ok);
    return ok;
}

void abortBlocking(const char* reason) {
    if (s_timer) xTimerStop(s_timer, 0);
    if (s_phase != Phase::Writing) return;
    latch(reason ? reason : "aborted");
    s_phase = Phase::Verifying;
    completeAndVerify(nullptr);
    release(false);
}

Phase       phase() { return s_phase; }
const char* error() { return s_err; }
bool        failed(const void* owner) { return owner && owner == s_lastOwner && s_failed; }
bool        isOwner(const void* owner) { return owner && owner == s_lastOwner; }

}  // namespace SpiffsUpload
