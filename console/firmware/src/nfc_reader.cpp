#include "nfc_reader.h"
#include "config.h"
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <string.h>
#include "spoolhard/serial_mirror.h"

// nfc_task: owns the PN532 (_pn / _spi) after begin(). 4 KB internal-DRAM
// stack (same as the scale's nfc_task): the deepest path is
// readPassiveTargetID / inDataExchange (shared global packet buffer in the
// lib) + a 64-byte page buffer + one Serial.printf. The first events log the
// stack high-water mark so the choice can be checked against /api/logs.
static constexpr uint32_t kNfcTaskStack = 4096;
static constexpr uint32_t kPollMs       = 200;
// Consecutive empty polls (~2 s) before a tag counts as removed and the
// UID debounce re-arms. Absorbs a single missed poll on a held tag.
static constexpr uint8_t  kRemovedMisses = 8;
static constexpr uint8_t  kEvqDepth      = 4;

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
    _pn->setPassiveActivationRetries(0x05);  // low retries so each poll stays short
    _available = true;

    _evq = xQueueCreate(kEvqDepth, sizeof(Event));
    if (!_evq ||
        xTaskCreatePinnedToCore(_taskBody, "nfc_task", kNfcTaskStack, this,
                                2, &_task, 1) != pdPASS) {
        Serial.println("[NFC] task/queue creation failed — reader disabled");
        _available = false;
        _task = nullptr;
    } else {
        Serial.println("[NFC] nfc_task started (PN532 owned by task)");
    }
}

// loopTask: deliver queued events.
void NfcReader::update() {
    if (!_available || !_evq) return;
    Event ev;
    while (xQueueReceive(_evq, &ev, 0) == pdTRUE) {
        SpoolTag tag;
        tag.uid_hex  = ev.uid;
        tag.format   = "Unknown";
        tag.tag_type = (ev.uidLen == 7) ? "NTAG2xx" : "Mifare";
        if (ev.url[0]) {
            tag.ndef_url = ev.url;
            SpoolTag::parseUrl(tag.ndef_url, tag);
        }
        if (_cb) _cb(tag);
    }
}

void NfcReader::_taskBody(void* arg) {
    auto* self = static_cast<NfcReader*>(arg);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        self->_pollStep();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(kPollMs));
    }
}

void NfcReader::_pollStep() {
    _polls.fetch_add(1);

    // Presence ping while a tag is held. An NTAG left in ACTIVE state after a
    // read does NOT answer the REQA that readPassiveTargetID sends, so polling
    // with InListPassiveTarget alone alternates found/not-found on a held tag
    // (the old reader hid this behind a 3 s re-fire timer). A cheap READ of
    // page 3 (CC) on the still-selected target is the reliable presence test.
    if (_present) {
        uint8_t cc[4];
        _pings.fetch_add(1);
        if (_pn->ntag2xx_ReadPage(3, cc)) { _misses = 0; return; }
    }

    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;
    if (!_pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
        if (_present && ++_misses >= kRemovedMisses) {
            _present = false;
            _lastUid[0] = 0;
            Serial.printf("[NFC] tag removed, re-armed (polls=%u pings=%u pageReads=%u events=%u)\n",
                          (unsigned)_polls.load(), (unsigned)_pings.load(),
                          (unsigned)_pageReads.load(), (unsigned)_events.load());
        }
        return;
    }
    _misses = 0;

    Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.uidLen = uidLen;
    int n = 0;
    for (uint8_t i = 0; i < uidLen && n + 3 < (int)sizeof(ev.uid); i++)
        n += snprintf(ev.uid + n, sizeof(ev.uid) - n, "%02X", uid[i]);

    // Debounce by UID BEFORE any page reads: same tag still on the reader
    // -> nothing to do. Removal (above) or a different UID re-arms.
    if (_present && strcmp(ev.uid, _lastUid) == 0) return;
    _present = true;
    strncpy(_lastUid, ev.uid, sizeof(_lastUid) - 1);

    if (uidLen == 7) _readNdefUrl(ev.url, sizeof(ev.url));

    _events.fetch_add(1);
    Serial.printf("[NFC] new tag %s uidLen=%u url=%s (polls=%u pings=%u pageReads=%u events=%u stackHWM=%u)\n",
                  ev.uid, (unsigned)uidLen, ev.url, (unsigned)_polls.load(),
                  (unsigned)_pings.load(), (unsigned)_pageReads.load(),
                  (unsigned)_events.load(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (xQueueSend(_evq, &ev, 0) != pdTRUE) {
        _dropped.fetch_add(1);
        Serial.println("[NFC] event queue full, dropped (retrying next poll)");
        _present = false;   // let the next poll retry instead of losing the tag
        _lastUid[0] = 0;
    }
}

// Reads pages 4..19 (the exact range the old reader read) with 4 READ
// commands of 16 bytes (= 4 pages each) and extracts a printable http(s)://
// run. If the first batched READ fails, falls back to the old single-page
// reads so a tag that rejects the raw batch still behaves as before.
bool NfcReader::_readNdefUrl(char* out, size_t outLen) {
    uint8_t raw[64];
    size_t have = 0;
    bool batchOk = true;
    for (uint8_t page = 4; page < 20; page += 4) {
        uint8_t cmd[2] = {0x30 /*MIFARE READ*/, page};
        uint8_t resp[18];
        uint8_t rlen = sizeof(resp);
        _pageReads.fetch_add(1);
        if (!_pn->inDataExchange(cmd, 2, resp, &rlen) || rlen < 16) {
            if (page == 4) batchOk = false;
            break;
        }
        memcpy(raw + have, resp, 16);
        have += 16;
    }
    if (!batchOk) {
        for (uint8_t page = 4; page < 20; page++) {
            uint8_t b[4];
            _pageReads.fetch_add(1);
            if (!_pn->ntag2xx_ReadPage(page, b)) break;
            memcpy(raw + have, b, 4);
            have += 4;
        }
        Serial.printf("[NFC] batch READ failed; single-page fallback read %u bytes\n",
                      (unsigned)have);
    }
    Serial.printf("[NFC] read %u bytes (%s) first8=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                  (unsigned)have, batchOk ? "batch" : "single",
                  have > 0 ? raw[0] : 0, have > 1 ? raw[1] : 0, have > 2 ? raw[2] : 0,
                  have > 3 ? raw[3] : 0, have > 4 ? raw[4] : 0, have > 5 ? raw[5] : 0,
                  have > 6 ? raw[6] : 0, have > 7 ? raw[7] : 0);
    const uint8_t* p = nullptr;
    static const char* const pats[2] = {"https://", "http://"};
    for (const char* pat : pats) {
        p = (const uint8_t*)memmem(raw, have, pat, strlen(pat));
        if (p) break;
    }
    if (!p) return false;
    size_t i = 0;
    while (p + i < raw + have && p[i] >= 32 && p[i] < 127 && i + 1 < outLen) {
        out[i] = (char)p[i];
        i++;
    }
    out[i] = 0;
    return i > 0;
}
