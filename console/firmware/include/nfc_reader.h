#pragma once
#include <Arduino.h>
#include <functional>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "spool_tag.h"

class Adafruit_PN532;
class SPIClass;

// PN532 SPI driver for the console's own on-device NFC reader. A separate
// instance lives on the scale — that one reaches us through ScaleLink's
// `TagStatus` messages.
//
// Threading: begin() initialises the PN532 on the caller's thread and then
// spawns `nfc_task`, which is the ONLY user of the PN532/SPI bus from then
// on (the console never writes/erases/emulates tags). The task polls,
// debounces by UID, reads the NDEF URL and posts POD events to a bounded
// queue; update() (loopTask) drains it and fires the callback there, because
// the callback touches LVGL and the spool store.
class NfcReader {
public:
    using Callback = std::function<void(const SpoolTag& tag)>;

    void begin();
    void update();

    bool available() const { return _available; }

    void onTag(Callback cb) { _cb = std::move(cb); }

    // ---- /api/debug/nfc support -------------------------------------------
    // Plain-POD record of the LAST detected tag, published by nfc_task under
    // a seqlock. debugSnapshot() copies it out from any task (no PN532 access).
    struct DebugRead {            // one READ command (batch = 16 B, single = 4 B)
        uint8_t page;
        uint8_t ok;
        uint8_t len;
        uint8_t raw[16];
    };
    struct DebugSnap {
        bool     valid;           // a tag has been detected since boot
        bool     present;         // currently considered on the reader
        bool     readable;
        uint8_t  uidLen;
        uint8_t  sak;
        uint16_t atqa;
        uint8_t  path;            // 0 none (not 7-byte UID), 1 batch, 2 single-page fallback, 3 failed
        uint8_t  nReads;
        uint16_t rawLen;
        uint32_t detectMs;        // millis() at detection
        uint32_t removedMs;       // millis() at logged removal (0 = not removed)
        char     uid[24];
        char     url[160];
        uint8_t  raw[64];         // pages 4..19 as read
        DebugRead reads[16];
    };
    static NfcReader* instance() { return s_instance; }
    bool debugSnapshot(DebugSnap& out) const;   // false if torn after retries

    // Observable counters (also logged on every event / re-arm).
    uint32_t pollCount()  const { return _polls.load(); }
    uint32_t pageReads()  const { return _pageReads.load(); }   // 4-page READ commands issued
    uint32_t eventCount() const { return _events.load(); }
    uint32_t droppedCount() const { return _dropped.load(); }

private:
    static NfcReader* s_instance;
    DebugSnap _dbgWork;                        // task-private working copy
    DebugSnap _dbgPub;                         // seqlock-protected published copy
    mutable std::atomic<uint32_t> _dbgSeq{0};  // odd = write in progress
    void _dbgPublish();

    // POD event passed through the queue (never Strings).
    struct Event {
        char    uid[24];
        char    url[160];
        uint8_t uidLen;
    };

    bool _available = false;
    Callback _cb;

    SPIClass*        _spi  = nullptr;
    Adafruit_PN532*  _pn   = nullptr;

    QueueHandle_t    _evq  = nullptr;
    TaskHandle_t     _task = nullptr;

    // Task-private debounce state.
    char     _lastUid[24] = {0};
    bool     _present     = false;
    bool     _readable    = false;   // first READ succeeded (NTAG-style)
    char     _unreadUid[24] = {0};
    uint32_t _lastSeenMs  = 0;
    uint8_t  _misses      = 0;

    std::atomic<uint32_t> _polls{0};
    std::atomic<uint32_t> _pings{0};
    std::atomic<uint32_t> _pageReads{0};
    std::atomic<uint32_t> _events{0};
    std::atomic<uint32_t> _dropped{0};

    static void _taskBody(void* arg);
    void _pollStep();
    bool _readNdefUrl(char* out, size_t outLen);
};
