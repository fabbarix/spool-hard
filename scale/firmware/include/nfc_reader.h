#pragma once
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include <atomic>

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

// Debug cache of the last tag the reader saw (GET /api/debug/nfc). Plain
// POD, published by nfc_task under a seqlock; HTTP reads a copy and never
// touches the PN532.
struct NfcDebug {
    uint8_t  uid[7];
    uint8_t  uid_len;
    uint8_t  sak;
    uint16_t atqa;
    uint8_t  raw[64];        // pages 4..19, 4 bytes each
    uint16_t page_mask;      // bit n set = page (4+n) was read into raw[]
    uint8_t  ndef_ok;        // last _readNdef result
    uint8_t  pingable;       // presence ping currently active
    uint32_t polls, pings, ping_fails, page_reads, events, removals, flaps_absorbed;
    uint32_t last_seen_ms;   // millis() of last presence confirmation
};

class NfcReader {
public:
    bool begin();                   // returns false if PN532 not present
    bool isAvailable() const        { return _available; }
    void update();                  // call in loop

    TagStatus getStatus() const     { return _status; }
    const SpoolTag& getLastTag() const { return _lastTag; }
    // Bumped every time update() rewrites _lastTag (nfc_task owner only).
    uint32_t tagGeneration() const  { return _tagGen; }

    bool writeTag(const uint8_t* uid, uint8_t uid_len, const String& ndef_message, const String& cookie);
    bool eraseTag(const uint8_t* uid, uint8_t uid_len);
    bool emulateTag(const String& url);
    // Seqlock-consistent copy of the debug cache; any task. False if a
    // writer kept interfering (caller reports 503).
    bool debugSnapshot(NfcDebug& out) const;

    void loadConfig();
    void saveConfig(bool available);

private:
    Adafruit_PN532 _pn532;
    bool       _available = false;
    TagStatus  _status    = TagStatus::Idle;
    SpoolTag   _lastTag;

    bool _readNdef(SpoolTag& tag);
    bool _isBambuLabTag(const uint8_t* uid, uint8_t uid_len);
    // Honest log label for the uid_len==7 heuristic (the wire is_bambulab
    // bool is unchanged: the console consumes it).
    static const char* _formatLabel(const SpoolTag& t) { return t.is_bambulab ? "ntag-7byte-uid" : "other-uid"; }
    void _setStatus(TagStatus s)    { _status = s; }

    bool _tagPresent = false;
    // Presence tracking. An NTAG left ACTIVE after a READ ignores the REQA
    // that readPassiveTargetID sends, so polling alone flaps on a held tag.
    // While _pingable (a readable NTAG is held) we READ page 3 on the still-
    // selected tag each poll; removal needs kRemovedMs of misses.
    bool     _pingable = false;
    uint8_t  _misses = 0;
    uint32_t _lastSeenMs = 0;
    NfcDebug _dbg = {};
    std::atomic<uint32_t> _dbgSeq{0};
    uint32_t _tagGen = 0;
    bool _uidMatchesPresent(const uint8_t* uid, uint8_t uid_len) const;

public:
    NfcReader();
};
