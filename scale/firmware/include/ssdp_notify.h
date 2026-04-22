#pragma once
#include <Arduino.h>
#include <AsyncUDP.h>

// Minimal SSDP-style NOTIFY broadcaster matching the format expected by the
// SpoolHard Console. The console does NOT use standard SSDP:
//   - listens on UDP 239.255.255.250:1990 (not 1900)
//   - passive only; never sends M-SEARCH
//   - Location: header must be a bare IPv4 string (no URL, no port, no path)
//   - USN: must exactly equal the configured scale name (if the user set one)
// See yanshay/SpoolEase:core/src/ssdp.rs and spool_scale.rs.
class SsdpNotify {
public:
    void begin(const String& deviceName, uint32_t intervalMs = 5000);
    void loop();  // call from main loop or task

private:
    AsyncUDP _udp;
    String   _deviceName;
    uint32_t _intervalMs = 5000;
    uint32_t _lastSend   = 0;

    void _sendNotify();
};
