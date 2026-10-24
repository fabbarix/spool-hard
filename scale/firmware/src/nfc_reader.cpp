#include "nfc_reader.h"
#include "config.h"
#include <Preferences.h>
#include <SPI.h>
#include "spoolhard/ring_log.h"
extern byte pn532_packetbuffer[];
#include "spoolhard/serial_mirror.h"

NfcReader::NfcReader()
    : _pn532(PN532_SCK_PIN, PN532_MISO_PIN, PN532_MOSI_PIN, PN532_SS_PIN) {}

bool NfcReader::begin() {
    loadConfig();
    _pn532.begin();
    uint32_t versiondata = _pn532.getFirmwareVersion();
    if (!versiondata) {
        Serial.println("[NFC] PN532 not found");
        _available = false;
        return false;
    }
    Serial.printf("[NFC] PN532 found, firmware v%d.%d\n",
                  (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
    _pn532.SAMConfig();
    _available = true;
    saveConfig(true);
    return true;
}

// Removal window in wall time (poll period is ~50 ms: 30 ms PN532 timeout +
// 20 ms task idle), plus a minimum miss count so one slow iteration can't
// satisfy the time test alone.
static constexpr uint32_t kRemovedMs      = 1500;
static constexpr uint8_t  kRemovedMinMiss = 5;

// Seqlock writer helpers: nfc_task is the only writer of _dbg.
#define DBG_BEGIN() _dbgSeq.fetch_add(1, std::memory_order_acq_rel)
#define DBG_END()   _dbgSeq.fetch_add(1, std::memory_order_release)

bool NfcReader::debugSnapshot(NfcDebug& out) const {
    for (int i = 0; i < 8; i++) {
        uint32_t s1 = _dbgSeq.load(std::memory_order_acquire);
        if (s1 & 1) { delay(1); continue; }
        memcpy(&out, &_dbg, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (_dbgSeq.load(std::memory_order_acquire) == s1) return true;
    }
    return false;
}

void NfcReader::update() {
    if (!_available) return;
    DBG_BEGIN(); _dbg.polls++; DBG_END();

    // Presence ping while a readable NTAG is held: READ page 3 (CC) on the
    // still-selected target. Runs only from nfc_task, which also owns the
    // write/erase/emulate commands (drained before update()), so it can
    // never interleave with them.
    if (_tagPresent && _pingable) {
        uint8_t cc[4];
        bool ok = _pn532.ntag2xx_ReadPage(3, cc);
        DBG_BEGIN();
        _dbg.pings++;
        if (ok) _dbg.last_seen_ms = millis(); else _dbg.ping_fails++;
        DBG_END();
        if (ok) { _misses = 0; _lastSeenMs = millis(); return; }
    }

    uint8_t uid[7];
    uint8_t uid_len = 0;

    // Poll timeout: 30 ms per tick. Was 100 ms (too long, blocked the
    // main loop) then 10 ms (too short, the PN532 needs ~25-30 ms to
    // power its RF field, broadcast the InListPassiveTarget command,
    // wait for the card response, and report — at 10 ms it would
    // routinely return "no card" even when a tag was held against the
    // antenna). 30 ms is the documented minimum for reliable
    // ISO14443A detection with this chip; loop budget cost is fine
    // since other per-tick work is well under 100 ms total.
    bool found = _pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 30);

    if (!found) {
        if (_tagPresent) {
            if (_misses < 255) _misses++;
            if (_misses >= kRemovedMinMiss && (millis() - _lastSeenMs) >= kRemovedMs) {
                _tagPresent = false;
                _pingable = false;
                _misses = 0;
                _setStatus(TagStatus::Idle);
                DBG_BEGIN(); _dbg.removals++; DBG_END();
                dlog("NFC", "Tag removed (polls=%u pings=%u pingFails=%u pageReads=%u events=%u)",
                     (unsigned)_dbg.polls, (unsigned)_dbg.pings, (unsigned)_dbg.ping_fails,
                     (unsigned)_dbg.page_reads, (unsigned)_dbg.events);
            }
        }
        return;
    }
    _misses = 0;
    _lastSeenMs = millis();

    // Same tag still on the reader — don't re-report.
    if (_tagPresent && uid_len == _lastTag.uid_len &&
        memcmp(uid, _lastTag.uid, uid_len) == 0) {
        DBG_BEGIN(); _dbg.flaps_absorbed++; _dbg.last_seen_ms = _lastSeenMs; DBG_END();
        return;
    }

    // PN532 InListPassiveTarget response: ATQA at [9..10], SAK at [11].
    uint16_t atqa = ((uint16_t)pn532_packetbuffer[9] << 8) | pn532_packetbuffer[10];
    uint8_t  sak  = pn532_packetbuffer[11];

    _tagPresent = true;
    _pingable = false;
    _tagGen++;
    _lastTag.uid_len = uid_len;
    memcpy(_lastTag.uid, uid, uid_len);
    _lastTag.is_bambulab = _isBambuLabTag(uid, uid_len);
    _lastTag.ndef_url = "";       // don't carry the previous tag's payload
    _lastTag.raw_message = "";

    DBG_BEGIN();
    memset(_dbg.uid, 0, sizeof(_dbg.uid));
    memcpy(_dbg.uid, uid, uid_len);
    _dbg.uid_len = uid_len;
    _dbg.sak = sak;
    _dbg.atqa = atqa;
    _dbg.page_mask = 0;
    memset(_dbg.raw, 0, sizeof(_dbg.raw));
    _dbg.ndef_ok = 0;
    _dbg.pingable = 0;
    _dbg.events++;
    _dbg.last_seen_ms = _lastSeenMs;
    DBG_END();

    _setStatus(TagStatus::FoundTagNowReading);

    bool ok = _readNdef(_lastTag);
    // Ping only a tag that answered a page READ (NTAG family); others keep
    // the poll-only path, covered by the time-based removal window.
    _pingable = ok || (_dbg.page_mask != 0);
    DBG_BEGIN(); _dbg.ndef_ok = ok; _dbg.pingable = _pingable; DBG_END();

    char hex[2 * 7 + 1] = {0};
    for (int i = 0; i < uid_len && i < 7; i++) snprintf(hex + 2 * i, 3, "%02X", uid[i]);
    char p4[9] = "--------";
    if (_dbg.page_mask & 1) snprintf(p4, sizeof(p4), "%02X%02X%02X%02X",
                                     _dbg.raw[0], _dbg.raw[1], _dbg.raw[2], _dbg.raw[3]);

    if (ok) {
        _setStatus(TagStatus::ReadSuccess);
        dlog("NFC", "Read tag UID=%s len=%u sak=0x%02X atqa=0x%04X format=%s page4=%s ndef=%uB",
             hex, (unsigned)uid_len, sak, atqa, _formatLabel(_lastTag), p4,
             (unsigned)_lastTag.raw_message.length());
    } else {
        _setStatus(TagStatus::Failure);
        dlog("NFC", "Tag read timeout/error UID=%s len=%u sak=0x%02X atqa=0x%04X format=%s page4=%s",
             hex, (unsigned)uid_len, sak, atqa, _formatLabel(_lastTag), p4);
    }
}

bool NfcReader::_readNdef(SpoolTag& tag) {
    // Read NDEF message from NTAG (pages 4+)
    // NTAG21x: 4 bytes per page, NDEF starts at page 4
    uint8_t buf[4];
    auto readPg = [&](uint8_t pg) -> bool {
        bool ok = _pn532.ntag2xx_ReadPage(pg, buf);
        if (ok && pg >= 4 && pg <= 19) {
            DBG_BEGIN();
            memcpy(_dbg.raw + (pg - 4) * 4, buf, 4);
            _dbg.page_mask |= (uint16_t)(1u << (pg - 4));
            _dbg.page_reads++;
            DBG_END();
        }
        return ok;
    };
    if (!readPg(4)) return false;

    // Check NDEF TLV header (0x03 = NDEF, next byte = length)
    if (buf[0] != 0x03) return false;

    uint16_t msg_len = buf[1];
    String ndef;
    ndef.reserve(msg_len);

    uint8_t page = 4;
    uint8_t byte_offset = 2;
    uint16_t read = 0;

    while (read < msg_len) {
        if (!readPg(page)) return false;
        for (int i = byte_offset; i < 4 && read < msg_len; i++, read++) {
            ndef += (char)buf[i];
        }
        byte_offset = 0;
        page++;
    }

    tag.raw_message = ndef;

    // Extract URL record (0xD1 01 len 55 url...)
    if (ndef.length() > 3 && (uint8_t)ndef[0] == 0xD1 && (uint8_t)ndef[2] == 0x55) {
        uint8_t prefix = ndef[3];
        const char* prefixes[] = {"", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://"};
        String url = (prefix < 17) ? prefixes[prefix] : "";
        url += ndef.substring(4);
        tag.ndef_url = url;
        tag.type = TagType::NTag;
    }

    return true;
}

bool NfcReader::_uidMatchesPresent(const uint8_t* uid, uint8_t uid_len) const {
    // Empty guard = caller didn't supply check_uid; otherwise the tag on
    // the antenna must be the one the caller meant to modify.
    if (uid_len == 0) return true;
    return _tagPresent && uid_len == _lastTag.uid_len &&
           memcmp(uid, _lastTag.uid, uid_len) == 0;
}

bool NfcReader::writeTag(const uint8_t* uid, uint8_t uid_len, const String& ndef_message, const String& cookie) {
    _setStatus(TagStatus::FoundTagNowWriting);

    if (!_uidMatchesPresent(uid, uid_len)) {
        Serial.println("[NFC] write: tag on reader doesn't match check_uid");
        _setStatus(TagStatus::Failure);
        return false;
    }

    // Build NDEF TLV: 0x03 <len> <ndef> 0xFE. Single-byte length form
    // only (<= 254 bytes), which also bounds the stack buffer.
    size_t len = ndef_message.length();
    if (len > 254) {
        Serial.printf("[NFC] write: message too long (%u > 254)\n", (unsigned)len);
        _setStatus(TagStatus::Failure);
        return false;
    }
    // Tag capacity from the NTAG capability container (page 3, byte 2 * 8).
    uint8_t cc[4];
    if (!_pn532.ntag2xx_ReadPage(3, cc) || cc[0] != 0xE1 || cc[2] == 0) {
        Serial.println("[NFC] write: can't read capability container");
        _setStatus(TagStatus::Failure);
        return false;
    }
    size_t capacity = (size_t)cc[2] * 8;
    size_t tlv_len = len + 3;
    if (tlv_len > capacity) {
        Serial.printf("[NFC] write: %u bytes exceed tag capacity %u\n",
                      (unsigned)tlv_len, (unsigned)capacity);
        _setStatus(TagStatus::Failure);
        return false;
    }

    uint8_t tlv[258 + 4] = {0};
    tlv[0] = 0x03;
    tlv[1] = (uint8_t)len;
    memcpy(tlv + 2, ndef_message.c_str(), len);
    tlv[2 + len] = 0xFE;

    uint8_t page = 4;
    for (size_t i = 0; i < tlv_len; i += 4) {
        if (!_pn532.ntag2xx_WritePage(page++, tlv + i)) {
            _setStatus(TagStatus::Failure);
            return false;
        }
    }

    _setStatus(TagStatus::WriteSuccess);
    return true;
}

bool NfcReader::eraseTag(const uint8_t* uid, uint8_t uid_len) {
    _setStatus(TagStatus::FoundTagNowErasing);
    if (!_uidMatchesPresent(uid, uid_len)) {
        Serial.println("[NFC] erase: tag on reader doesn't match check_uid");
        _setStatus(TagStatus::Failure);
        return false;
    }
    // Write empty NDEF TLV terminator
    uint8_t buf[4] = {0x03, 0x00, 0xFE, 0x00};
    if (!_pn532.ntag2xx_WritePage(4, buf)) {
        _setStatus(TagStatus::Failure);
        return false;
    }
    _setStatus(TagStatus::EraseSuccess);
    return true;
}

bool NfcReader::emulateTag(const String& url) {
    // PN532 card emulation — basic ISO14443-4 emulation
    // Full emulation requires TgInitAsTarget — placeholder for now
    Serial.printf("[NFC] Emulate tag URL: %s\n", url.c_str());
    // AsTarget() puts PN532 into card emulation mode (no args in this lib version)
    // Returns 0 on success
    _pingable = false;   // PN532 leaves initiator mode: no ping until a fresh read
    uint8_t result = _pn532.AsTarget();
    if (result != 0) {
        Serial.println("[NFC] Emulate: tag not fully read");
        return false;
    }
    Serial.println("[NFC] Emulate: tag fully read");
    return true;
}

bool NfcReader::_isBambuLabTag(const uint8_t* uid, uint8_t uid_len) {
    // Bambu Lab tags are NTAG215 with a specific UID prefix — heuristic
    // Actual detection happens by reading manufacturer data block 0
    return uid_len == 7;
}

void NfcReader::loadConfig() {
    Preferences prefs;
    prefs.begin(NVS_NS_NFC, true);
    _available = prefs.getBool(NVS_KEY_NFC_AVAIL, false);
    prefs.end();
}

void NfcReader::saveConfig(bool available) {
    Preferences prefs;
    prefs.begin(NVS_NS_NFC, false);
    prefs.putBool(NVS_KEY_NFC_AVAIL, available);
    prefs.end();
}
