#pragma once
#include <Arduino.h>
#include <functional>
#include "spool_tag.h"

class Adafruit_PN532;
class SPIClass;

// PN532 SPI driver for the console's own on-device NFC reader. A separate
// instance lives on the scale — that one reaches us through ScaleLink's
// `TagStatus` messages.
class NfcReader {
public:
    using Callback = std::function<void(const SpoolTag& tag)>;

    void begin();
    void update();

    bool available() const { return _available; }

    void onTag(Callback cb) { _cb = std::move(cb); }

private:
    bool _available = false;
    uint32_t _lastPollMs = 0;
    String _lastUid;
    Callback _cb;

    SPIClass*        _spi  = nullptr;
    Adafruit_PN532*  _pn   = nullptr;

    bool _pollOnce(SpoolTag& tag);
    static String _uidHex(const uint8_t* uid, uint8_t len);
};
