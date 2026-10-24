#include "sdcard.h"
#include "config.h"
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "spoolhard/serial_mirror.h"

SdCard g_sd;

static SemaphoreHandle_t s_sdMtx = nullptr;
static portMUX_TYPE s_sdMtxInit = portMUX_INITIALIZER_UNLOCKED;

// Created from SdCard::begin() (single-threaded setup) and lazily here as a
// safety net; the critical section makes lazy creation race-free.
static void _sdMtxEnsure() {
    if (s_sdMtx) return;
    SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    portENTER_CRITICAL(&s_sdMtxInit);
    if (!s_sdMtx) { s_sdMtx = m; m = nullptr; }
    portEXIT_CRITICAL(&s_sdMtxInit);
    if (m) vSemaphoreDelete(m);
}

bool sdLock(uint32_t timeout_ms) {
    _sdMtxEnsure();
    if (!s_sdMtx) return false;
    if (xSemaphoreTakeRecursive(s_sdMtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) return true;
    Serial.printf("[SD] lock timeout (%lu ms) from task %s\n",
                  (unsigned long)timeout_ms, pcTaskGetName(nullptr));
    return false;
}

void sdUnlock() {
    if (s_sdMtx) xSemaphoreGiveRecursive(s_sdMtx);
}

static SPIClass* s_sdSpi = nullptr;

void SdCard::begin() {
    _sdMtxEnsure();
    SdLock lk(2000);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);  // deselect

    s_sdSpi = new SPIClass(FSPI);
    s_sdSpi->begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    _tryMount();
}

void SdCard::update() {
    uint32_t now = millis();
    if (now - _lastCheck < 2000) return;
    _lastCheck = now;

    // Mount/unmount (SD.begin/SD.end) must not race file I/O in other
    // tasks. Short timeout: if someone is mid-I/O, retry in 2 s.
    SdLock lk(20);
    if (!lk) { _lastCheck = now - 1500; return; }
    if (!_mounted) {
        _tryMount();
    } else {
        // Cheap liveness check: read totalBytes. If the card was yanked, the
        // SD driver will start returning 0.
        uint64_t t = SD.totalBytes();
        if (t == 0) {
            _unmount();
        }
    }
}

void SdCard::_tryMount() {
    // SD.begin(cs, spi, freq, mountpoint)
    if (!SD.begin(SD_CS_PIN, *s_sdSpi, 20000000, SD_MOUNT)) {
        return;  // no card or mount failed — try again next tick
    }
    uint8_t type = SD.cardType();
    if (type == CARD_NONE) {
        SD.end();
        return;
    }
    _mounted = true;
    _refreshUsage();
    Serial.printf("[SD] Mounted: type=%u total=%llu MB used=%llu MB\n",
                  type, _total / (1024 * 1024), _used / (1024 * 1024));
}

void SdCard::_unmount() {
    SD.end();
    _mounted = false;
    _total = 0;
    _used  = 0;
    Serial.println("[SD] Unmounted (card removed or failed)");
}

void SdCard::_refreshUsage() {
    _total = SD.totalBytes();
    _used  = SD.usedBytes();
}
