#pragma once
#include <Arduino.h>

// microSD slot on the WT32-SC01-Plus (SPI bus: CS=41, SCK=39, MOSI=40, MISO=38).
// Mounted at SD_MOUNT ("/sd") if a card is present at boot. The mount is
// checked on a schedule so hot-swap is detected without requiring a reboot.
class SdCard {
public:
    void begin();
    void update();    // poll every 2s for card insert/remove

    bool isMounted() const   { return _mounted; }
    uint64_t totalBytes() const { return _total; }
    uint64_t usedBytes() const  { return _used; }

private:
    bool _mounted = false;
    uint64_t _total = 0;
    uint64_t _used  = 0;
    uint32_t _lastCheck = 0;

    void _tryMount();
    void _unmount();
    void _refreshUsage();
};

extern SdCard g_sd;
