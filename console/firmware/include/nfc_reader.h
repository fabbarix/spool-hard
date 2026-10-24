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

    // Observable counters (also logged on every event / re-arm).
    uint32_t pollCount()  const { return _polls.load(); }
    uint32_t pageReads()  const { return _pageReads.load(); }   // 4-page READ commands issued
    uint32_t eventCount() const { return _events.load(); }

private:
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
