#include "sdcard.h"
#include "config.h"
#include <SPI.h>
#include <SD.h>

SdCard g_sd;

static SPIClass* s_sdSpi = nullptr;

void SdCard::begin() {
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
