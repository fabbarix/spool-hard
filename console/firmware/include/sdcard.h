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

// ── Global SD mutex ────────────────────────────────────────────────
// The Arduino SD/FAT stack is not thread-safe and several tasks touch the
// card (loopTask, AsyncTCP handlers, scale_link, analyse/FTP tasks). All
// SD file I/O must hold this recursive mutex.
//
// LOCK ORDER (outermost first; never acquire an earlier lock while
// holding a later one):
//     CrashLogger::s_mtx  ->  SdLock  ->  (leaf) RingLog mutex / Serial
// Never take the LVGL lock, a FreeRTOS queue/other mutex, or perform a
// network send / long FTP read while holding the SD lock. Hold it only for
// short bounded operations (chunked streams re-take it per chunk).
// Acquire has a finite timeout; on timeout the guard is false -> the
// caller must log+skip (never block forever).
bool sdLock(uint32_t timeout_ms = 500);
void sdUnlock();

class SdLock {
public:
    explicit SdLock(uint32_t timeout_ms = 500) : _ok(sdLock(timeout_ms)) {}
    ~SdLock() { if (_ok) sdUnlock(); }
    SdLock(const SdLock&) = delete;
    SdLock& operator=(const SdLock&) = delete;
    explicit operator bool() const { return _ok; }
    bool ok() const { return _ok; }
private:
    bool _ok;
};
