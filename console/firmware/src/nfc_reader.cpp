#include "nfc_reader.h"
#include "config.h"
#include <SPI.h>
#include <Adafruit_PN532.h>

void NfcReader::begin() {
    pinMode(PN532_IRQ_PIN, INPUT_PULLUP);

    // Dedicated SPI bus so we don't interfere with anything else.
    _spi = new SPIClass(HSPI);
    _spi->begin(PN532_SCK_PIN, PN532_MISO_PIN, PN532_MOSI_PIN, PN532_SS_PIN);
    _pn  = new Adafruit_PN532(PN532_SS_PIN, _spi);

    _pn->begin();
    uint32_t ver = _pn->getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] PN532 not found on extended header");
        _available = false;
        return;
    }
    Serial.printf("[NFC] PN532 fw 0x%08X\n", ver);
    _pn->SAMConfig();
    _pn->setPassiveActivationRetries(0x05);  // low retries so update() is non-blocking
    _available = true;
}

void NfcReader::update() {
    if (!_available) return;
    uint32_t now = millis();
    if (now - _lastPollMs < 200) return;   // 5 Hz poll
    _lastPollMs = now;

    SpoolTag tag;
    if (!_pollOnce(tag)) return;

    // Debounce: ignore repeat reads of the same UID within 3 seconds.
    static uint32_t lastFireMs = 0;
    if (tag.uid_hex == _lastUid && (now - lastFireMs) < 3000) return;
    _lastUid = tag.uid_hex;
    lastFireMs = now;

    if (_cb) _cb(tag);
}

bool NfcReader::_pollOnce(SpoolTag& tag) {
    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;
    if (!_pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) return false;

    tag.uid_hex = _uidHex(uid, uidLen);
    tag.format  = "Unknown";
    tag.tag_type = (uidLen == 7) ? "NTAG2xx" : "Mifare";

    // Attempt to read an NDEF URI record off an NTAG (pages 4..).
    // This is intentionally minimal — full Bambu Lab and OpenPrintTag parsing
    // lives in a later milestone.
    if (uidLen == 7) {
        uint8_t buf[16];
        String raw;
        for (uint8_t page = 4; page < 20; page++) {
            if (!_pn->ntag2xx_ReadPage(page, buf)) break;
            for (int i = 0; i < 4; i++) raw += (char)buf[i];
        }
        // Look for the NDEF URI prefix byte 0x03 (well-known type U) — tolerant scan.
        int uStart = raw.indexOf("https://");
        if (uStart < 0) uStart = raw.indexOf("http://");
        if (uStart >= 0) {
            int end = uStart;
            while (end < (int)raw.length() && raw[end] >= 32 && raw[end] < 127) end++;
            tag.ndef_url = raw.substring(uStart, end);
            SpoolTag::parseUrl(tag.ndef_url, tag);
        }
    }
    return true;
}

String NfcReader::_uidHex(const uint8_t* uid, uint8_t len) {
    char buf[32];
    int n = 0;
    for (uint8_t i = 0; i < len && n + 2 < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%02X", uid[i]);
    }
    return String(buf);
}

// (URL parsing now lives in SpoolTag::parseUrl — shared with the scale-
//  forwarded tag pipeline so both readers behave identically.)
