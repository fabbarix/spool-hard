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
    void update();   // prunes stale entries + slow auto-probe; call periodically

    const std::vector<Entry>& entries() const { return _entries; }

    // Force an immediate active SSDP M-SEARCH on both 1990 and 1900. Use
    // when the user taps a Refresh button or right after WiFi (re)connect.
    void probe();

    // Fired every time a Bambu announcement is parsed (NOTIFY or M-SEARCH
    // response). Wired by main.cpp into BambuManager so it can pick up an
    // IP change for an already-configured printer and trigger a reconnect
    // — answering the "I powered the printer on, it's now on a fresh DHCP
    // lease, how do I get the console to notice" use case without forcing
    // the user to edit the IP by hand.
    using OnSeenCb = std::function<void(const Entry&)>;
    void setOnSeen(OnSeenCb cb) { _onSeen = std::move(cb); }

private:
    // Listeners live in ssdp_hub; we just subscribe. See ssdp_hub.h.
    std::vector<Entry> _entries;
    uint32_t _lastPruneMs = 0;
    uint32_t _lastProbeMs = 0;
    OnSeenCb _onSeen;

    void _onAnnounce(const SsdpListener::Announce& a, const String& extra_model);
    static String _parseModel(const String& raw_packet);
};

extern BambuDiscovery g_bambu_discovery;
