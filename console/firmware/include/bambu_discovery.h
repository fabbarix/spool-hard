#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <vector>
#include "ssdp_listener.h"

/**
 * Passive Bambu SSDP listener. Per Bambu's wiki + forum, their printers use
 * *non-standard* SSDP ports: X1 / X1C / H2D on 239.255.255.250:1990 and P1P
 * on the standard 239.255.255.250:1900. We bind both so any Bambu on the
 * LAN is caught. The :1990 listener also hears the SpoolHard scale's NOTIFY
 * packets but they're discarded by the URN filter.
 *
 * Entries are kept for DISCOVERY_TTL_MS after the last NOTIFY so a printer
 * that powers off stops appearing.
 */
class BambuDiscovery {
public:
    struct Entry {
        String    serial;        // from USN
        IPAddress ip;            // from Location
        String    model;         // optional hint, e.g. "3DPrinter-X1-Carbon"
        uint32_t  last_seen_ms;
    };

    static constexpr uint32_t DISCOVERY_TTL_MS = 5 * 60 * 1000;  // 5 min

    void begin();
    void update();   // prunes stale entries; call periodically

    const std::vector<Entry>& entries() const { return _entries; }

private:
    // Listeners live in ssdp_hub; we just subscribe. See ssdp_hub.h.
    std::vector<Entry> _entries;
    uint32_t _lastPruneMs = 0;

    void _onAnnounce(const SsdpListener::Announce& a, const String& extra_model);
    static String _parseModel(const String& raw_packet);
};

extern BambuDiscovery g_bambu_discovery;
