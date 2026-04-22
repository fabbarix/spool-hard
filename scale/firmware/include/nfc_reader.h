#pragma once
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>

enum class TagType { Unknown, NTag, MifareClassic1K, MifareClassic4K };

struct SpoolTag {
    uint8_t  uid[7];
    uint8_t  uid_len;
    TagType  type;
    bool     is_bambulab;
    String   ndef_url;      // for URL/NDEF tags
    String   raw_message;   // raw NDEF payload
};

enum class TagStatus {
    Idle,
    FoundTagNowReading,
    FoundTagNowWriting,
    FoundTagNowErasing,
    WriteSuccess,
    ReadSuccess,
    EraseSuccess,
    Failure,
    ReadTimeout,
};

class NfcReader {
public:
    bool begin();                   // returns false if PN532 not present
    bool isAvailable() const        { return _available; }
    void update();                  // call in loop

    TagStatus getStatus() const     { return _status; }
    const SpoolTag& getLastTag() const { return _lastTag; }

    bool writeTag(const uint8_t* uid, uint8_t uid_len, const String& ndef_message, const String& cookie);
    bool eraseTag(const uint8_t* uid, uint8_t uid_len);
    bool emulateTag(const String& url);

    void loadConfig();
    void saveConfig(bool available);

private:
    Adafruit_PN532 _pn532;
    bool       _available = false;
    TagStatus  _status    = TagStatus::Idle;
    SpoolTag   _lastTag;

    bool _readNdef(SpoolTag& tag);
    bool _isBambuLabTag(const uint8_t* uid, uint8_t uid_len);
    void _setStatus(TagStatus s)    { _status = s; }

    bool _tagPresent = false;

public:
    NfcReader();
};
