#include "nfc_reader.h"
#include "config.h"
#include <Preferences.h>
#include <SPI.h>

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

void NfcReader::update() {
    if (!_available) return;

    uint8_t uid[7];
    uint8_t uid_len = 0;

    bool found = _pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 100);

    if (!found) {
        if (_tagPresent) {
            _tagPresent = false;
            _setStatus(TagStatus::Idle);
            Serial.println("[NFC] Tag removed");
        }
        return;
    }

    // Same tag still on the reader — don't re-report.
    if (_tagPresent && uid_len == _lastTag.uid_len &&
        memcmp(uid, _lastTag.uid, uid_len) == 0) {
        return;
    }

    _tagPresent = true;
    _lastTag.uid_len = uid_len;
    memcpy(_lastTag.uid, uid, uid_len);
    _lastTag.is_bambulab = _isBambuLabTag(uid, uid_len);

    _setStatus(TagStatus::FoundTagNowReading);

    if (_readNdef(_lastTag)) {
        _setStatus(TagStatus::ReadSuccess);
        Serial.printf("[NFC] Read tag UID: ");
        for (int i = 0; i < uid_len; i++) Serial.printf("%02X:", uid[i]);
        Serial.println();
    } else {
        _setStatus(TagStatus::Failure);
        Serial.println("[NFC] Tag read timeout/error");
    }
}

bool NfcReader::_readNdef(SpoolTag& tag) {
    // Read NDEF message from NTAG (pages 4+)
    // NTAG21x: 4 bytes per page, NDEF starts at page 4
    uint8_t buf[4];
    if (!_pn532.ntag2xx_ReadPage(4, buf)) return false;

    // Check NDEF TLV header (0x03 = NDEF, next byte = length)
    if (buf[0] != 0x03) return false;

    uint16_t msg_len = buf[1];
    String ndef;
    ndef.reserve(msg_len);

    uint8_t page = 4;
    uint8_t byte_offset = 2;
    uint16_t read = 0;

    while (read < msg_len) {
        if (!_pn532.ntag2xx_ReadPage(page, buf)) return false;
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

bool NfcReader::writeTag(const uint8_t* uid, uint8_t uid_len, const String& ndef_message, const String& cookie) {
    _setStatus(TagStatus::FoundTagNowWriting);

    // Build NDEF TLV: 0x03 <len> <ndef> 0xFE
    uint8_t len = ndef_message.length();
    String tlv;
    tlv += (char)0x03;
    tlv += (char)len;
    tlv += ndef_message;
    tlv += (char)0xFE;

    uint8_t page = 4;
    for (int i = 0; i < (int)tlv.length(); i += 4) {
        uint8_t buf[4] = {0};
        for (int j = 0; j < 4 && (i + j) < (int)tlv.length(); j++)
            buf[j] = tlv[i + j];
        if (!_pn532.ntag2xx_WritePage(page++, buf)) {
            _setStatus(TagStatus::Failure);
            return false;
        }
    }

    _setStatus(TagStatus::WriteSuccess);
    return true;
}

bool NfcReader::eraseTag(const uint8_t* uid, uint8_t uid_len) {
    _setStatus(TagStatus::FoundTagNowErasing);
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
