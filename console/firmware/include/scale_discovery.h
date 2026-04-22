#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <vector>
#include "ssdp_listener.h"

/**
 * Passive listener for SpoolHard-Scale SSDP announcements on port 1990.
 * Mirrors BambuDiscovery's shape but filters on SCALE_SSDP_URN. ScaleLink
 * pairs with exactly one scale; this class keeps the full list of scales
 * seen recently so the UI can show all devices on the LAN and offer a link
 * to each one's own config page.
 */
class ScaleDiscovery {
public:
    struct Entry {
        String    name;          // from USN (user-set scale hostname)
        IPAddress ip;
        uint32_t  last_seen_ms;
    };

    static constexpr uint32_t DISCOVERY_TTL_MS = 5 * 60 * 1000;  // 5 min

    void begin();
    void update();

    const std::vector<Entry>& entries() const { return _entries; }

private:
    std::vector<Entry> _entries;
    uint32_t _lastPruneMs = 0;

    void _onAnnounce(const SsdpListener::Announce& a);
};

extern ScaleDiscovery g_scale_discovery;
