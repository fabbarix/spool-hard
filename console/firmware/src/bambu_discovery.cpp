#include "bambu_discovery.h"
#include "config.h"
#include "ssdp_hub.h"

BambuDiscovery g_bambu_discovery;

void BambuDiscovery::begin() {
    auto onAnnounce = [this](const SsdpListener::Announce& a) {
        // Only Bambu devices — filter by URN substring so minor firmware
        // variations (capitalisation, version suffix) still match. On :1990
        // this harmlessly ignores the scale's own NOTIFY packets; on :1900
        // it ignores any standard UPnP chatter on the LAN.
        String urn = a.urn; urn.toLowerCase();
        if (urn.indexOf(BAMBU_SSDP_URN_TAG) < 0) return;
        _onAnnounce(a, "");
    };
    g_ssdp_1990.subscribe(onAnnounce);  // X1 / X1C / H2D
    g_ssdp_1900.subscribe(onAnnounce);  // P1P
}

void BambuDiscovery::update() {
    uint32_t now = millis();

    // Slow auto-probe so a printer that powers on between NOTIFY broadcasts
    // (Bambu sends one every ~30 s) is still picked up within ~1 minute
    // without the user having to tap Refresh. Cheap — two UDP packets.
    if (now - _lastProbeMs > 60000) {
        _lastProbeMs = now;
        ssdp_hub_probe();
    }

    if (now - _lastPruneMs < 30000) return;
    _lastPruneMs = now;

    size_t before = _entries.size();
    _entries.erase(
        std::remove_if(_entries.begin(), _entries.end(),
            [now](const Entry& e) { return (now - e.last_seen_ms) > DISCOVERY_TTL_MS; }),
        _entries.end()
    );
    if (_entries.size() != before) {
        Serial.printf("[BambuDisc] Pruned %u stale entries\n",
                      (unsigned)(before - _entries.size()));
    }
}

void BambuDiscovery::probe() {
    _lastProbeMs = millis();   // reset auto-probe clock
    ssdp_hub_probe();
}

void BambuDiscovery::_onAnnounce(const SsdpListener::Announce& a, const String& model) {
    if (a.usn.isEmpty() || (uint32_t)a.ip == 0) return;

    uint32_t now = millis();
    for (auto& e : _entries) {
        if (e.serial == a.usn) {
            e.ip = a.ip;
            e.last_seen_ms = now;
            if (model.length()) e.model = model;
            if (_onSeen) _onSeen(e);
            return;
        }
    }
    Entry e;
    e.serial = a.usn;
    e.ip     = a.ip;
    e.model  = model;
    e.last_seen_ms = now;
    _entries.push_back(e);
    Serial.printf("[BambuDisc] New printer: %s @ %s\n",
                  a.usn.c_str(), a.ip.toString().c_str());
    if (_onSeen) _onSeen(_entries.back());
}
