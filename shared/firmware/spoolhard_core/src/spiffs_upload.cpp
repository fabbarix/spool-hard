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
#include <atomic>

namespace SpiffsUpload {
namespace {

constexpr size_t kSector = 4096;
constexpr size_t kStage  = 4096;   // staged in RAM before the FS is touched
const char kMarker[] = "SPOOLHARD-PRODUCT=";

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
uint32_t       s_gen = 0;              // token generator
uint32_t       s_tok = 0;              // active upload (0 = none)
uint32_t       s_lastTok = 0;          // most recent upload (for results)
char           s_err[96] = "";
bool           s_failed = false;
bool           s_committed = false;    // FS unmounted + Update.begin done
size_t         s_written = 0;
size_t         s_imageSize = 0;
char           s_expected[65] = "";
char           s_sig[48] = "";
uint8_t*       s_stage = nullptr;
size_t         s_stageLen = 0;
uint32_t       s_stallMs = 0;
mbedtls_sha256_context s_sha;
TimerHandle_t  s_timer = nullptr;
bool           s_reboot = false;
std::atomic<bool> s_claimed{false};    // finish/abandon runs exactly once
TaskFunction_t s_pendingFn = nullptr;  // spawn retry (OOM)
const char*    s_pendingName = nullptr;

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

void freeStage() {
    free(s_stage);
    s_stage = nullptr;
    s_stageLen = 0;
}

// Point of no return: unmount, start the raw write, flush staged bytes.
bool commit() {
    SPIFFS.end();                       // never raw-write a mounted FS
    if (!Update.begin(s_imageSize, U_SPIFFS)) {
        Serial.printf("[SpiffsUp] Update.begin failed: %s\n", Update.errorString());
        SPIFFS.begin(true);
        latch("Update.begin failed");
        freeStage();
        return false;
    }
    s_committed = true;
    if (s_stageLen) {
        size_t n = Update.write(s_stage, s_stageLen);
        if (n != s_stageLen) { latch("short write to flash"); freeStage(); return false; }
    }
    freeStage();
    return true;
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
// Leaves SPIFFS mounted (formatted if it had been erased and failed).
bool completeAndVerify(const char* clientSha) {
    const char* why = nullptr;
    uint8_t digest[32];
    mbedtls_sha256_finish(&s_sha, digest);
    mbedtls_sha256_free(&s_sha);
    bool ok = !s_failed;

    if (ok && !s_committed && !commit()) ok = false;
    if (ok) {
        if (!Update.end(true)) { latch("Update.end failed"); ok = false; }
    } else if (s_committed) {
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

    if (!ok && s_committed) {
        Serial.printf("[SpiffsUp] FAILED (%s) - formatting SPIFFS\n", s_err);
        SPIFFS.end();
        SPIFFS.format();
        SPIFFS.begin(true);
    } else if (!ok) {
        Serial.printf("[SpiffsUp] rejected before erase (%s) - old FS intact\n", s_err);
    }
    return ok;
}

void release(bool ok) {
    freeStage();
    s_tok = 0;
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
    Serial.println("[SpiffsUp] upload stalled - abandoning");
    latch("upload stalled");
    completeAndVerify(nullptr);
    release(false);
    vTaskDelete(nullptr);
}

bool trySpawn(TaskFunction_t fn, const char* name) {
    return xTaskCreate(fn, name, 6144, nullptr, 1, nullptr) == pdPASS;
}

// Claim the (single) completion slot and start it. Never runs the work
// inline: on OOM the timer retries every second (FS stays quiesced).
bool spawn(TaskFunction_t fn, const char* name) {
    if (s_claimed.exchange(true)) return false;
    s_phase = Phase::Verifying;
    if (!trySpawn(fn, name)) {
        Serial.println("[SpiffsUp] task create failed, retrying in 1 s");
        s_pendingFn = fn; s_pendingName = name;
        if (s_timer) xTimerChangePeriod(s_timer, pdMS_TO_TICKS(1000), 0);
    }
    return true;
}

void onStall(TimerHandle_t) {   // timer-service task: spawn only, no work
    if (s_pendingFn) {
        if (trySpawn(s_pendingFn, s_pendingName)) s_pendingFn = nullptr;
        else xTimerChangePeriod(s_timer, pdMS_TO_TICKS(1000), 0);
    } else if (s_phase == Phase::Writing) {
        spawn(abandonTask, "spiffs_abn");
    }
}

void touch() {
    if (s_timer && s_stallMs) xTimerChangePeriod(s_timer, pdMS_TO_TICKS(s_stallMs), 0);
}

}  // namespace

size_t partitionSize() {
    const esp_partition_t* p = part();
    return p ? p->size : 0;
}

bool tryLockFs() { return xSemaphoreTake(fsSem(), 0) == pdTRUE; }
void unlockFs()  { xSemaphoreGive(fsSem()); }

uint32_t begin(const char** err, const char* productSig, size_t imageSize, uint32_t stallMs) {
    const esp_partition_t* p = part();
    if (!p) { *err = "no spiffs partition"; return 0; }
    size_t sz = imageSize ? imageSize : p->size;
    if (sz > p->size) { *err = "image larger than frontend partition"; return 0; }
    if (xSemaphoreTake(fsSem(), pdMS_TO_TICKS(2000)) != pdTRUE) {
        *err = "frontend upload already in progress";
        return 0;
    }
    s_failed = false; s_err[0] = 0; s_written = 0; s_imageSize = sz;
    s_expected[0] = 0; s_reboot = false; s_committed = false;
    s_pendingFn = nullptr; s_claimed = false; s_stallMs = stallMs;
    freeStage();
    s_sig[0] = 0;
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    ++s_gen;
    if (s_gen == 0) ++s_gen;
    s_tok = s_lastTok = s_gen;
    s_phase = Phase::Writing;
    bool stage = productSig && *productSig && strlen(productSig) < sizeof(s_sig);
    if (stage) {
        strcpy(s_sig, productSig);
        s_stage = (uint8_t*)malloc(kStage);
        stage = s_stage != nullptr;
    }
    if (!stage && !commit()) {
        mbedtls_sha256_free(&s_sha);
        s_tok = 0;
        s_phase = Phase::Failed;
        xSemaphoreGive(fsSem());
        *err = "Update.begin failed";
        return 0;
    }
    if (stallMs && !s_timer)
        s_timer = xTimerCreate("spiffs_wd", pdMS_TO_TICKS(stallMs), pdFALSE, nullptr, onStall);
    touch();
    return s_tok;
}

bool owns(uint32_t tok) { return tok && s_tok == tok && s_phase == Phase::Writing; }

void setExpectedSha256(uint32_t tok, const char* hex) {
    if (!owns(tok) || !hex) return;
    strncpy(s_expected, hex, 64);
    s_expected[64] = 0;
}

bool write(uint32_t tok, const uint8_t* data, size_t len) {
    if (!owns(tok) || s_failed) return false;
    if (s_written + len > s_imageSize) { latch("image larger than partition"); return false; }
    mbedtls_sha256_update(&s_sha, data, len);
    s_written += len;
    touch();
    if (!s_committed) {
        size_t take = kStage - s_stageLen < len ? kStage - s_stageLen : len;
        memcpy(s_stage + s_stageLen, data, take);
        s_stageLen += take; data += take; len -= take;
        size_t sl = strlen(s_sig), ml = sizeof(kMarker) - 1;
        bool decide = s_stageLen == kStage;
        for (size_t i = 0; i + ml <= s_stageLen; ++i) {
            if (memcmp(s_stage + i, kMarker, ml) != 0) continue;
            if (i + sl <= s_stageLen) {
                if (memcmp(s_stage + i, s_sig, sl) != 0) {
                    // FS not touched yet: the old frontend survives.
                    latch("wrong product - signature mismatch (old frontend kept)");
                    return false;
                }
                decide = true;       // right product: safe to erase
            }
            break;
        }
        if (decide && !commit()) return false;
        if (!s_committed) return true;   // still staging
    }
    if (len) {
        size_t n = Update.write(const_cast<uint8_t*>(data), len);
        if (n != len) {
            Serial.printf("[SpiffsUp] short write %u/%u: %s\n", (unsigned)n, (unsigned)len, Update.errorString());
            latch("short write to flash");
            return false;
        }
    }
    return true;
}

void fail(uint32_t tok, const char* reason) {
    if (owns(tok)) latch(reason);
}

void finish(uint32_t tok, bool reboot) {
    if (!owns(tok)) return;
    if (s_timer) xTimerStop(s_timer, 0);
    s_reboot = reboot;
    spawn(finishTask, "spiffs_fin");
}

bool finishBlocking(uint32_t tok, const char* expectedSha256Hex) {
    if (!owns(tok)) return false;
    if (s_timer) xTimerStop(s_timer, 0);
    if (s_claimed.exchange(true)) return false;
    s_phase = Phase::Verifying;
    bool ok = completeAndVerify(expectedSha256Hex);
    release(ok);
    return ok;
}

void abortBlocking(uint32_t tok, const char* reason) {
    if (!owns(tok)) return;
    if (s_timer) xTimerStop(s_timer, 0);
    if (s_claimed.exchange(true)) return;
    latch(reason ? reason : "aborted");
    s_phase = Phase::Verifying;
    completeAndVerify(nullptr);
    release(false);
}

Phase       phase() { return s_phase; }
const char* error() { return s_err; }
bool        failed(uint32_t tok) { return tok && tok == s_lastTok && s_failed; }
bool        isCurrent(uint32_t tok) { return tok && tok == s_lastTok; }

}  // namespace SpiffsUpload
