#include "scale_discovery.h"
#include "config.h"
#include "ssdp_hub.h"

ScaleDiscovery g_scale_discovery;

void ScaleDiscovery::begin() {
    g_ssdp_1990.subscribe([this](const SsdpListener::Announce& a) {
        if (a.urn != SCALE_SSDP_URN) return;
        _onAnnounce(a);
    });
}

void ScaleDiscovery::update() {
    uint32_t now = millis();
    if (now - _lastPruneMs < 30000) return;
    _lastPruneMs = now;
    _entries.erase(
        std::remove_if(_entries.begin(), _entries.end(),
            [now](const Entry& e) { return (now - e.last_seen_ms) > DISCOVERY_TTL_MS; }),
        _entries.end()
    );
}

void ScaleDiscovery::_onAnnounce(const SsdpListener::Announce& a) {
    if (a.usn.isEmpty() || (uint32_t)a.ip == 0) return;
    uint32_t now = millis();
    for (auto& e : _entries) {
        if (e.name == a.usn) {
            e.ip = a.ip;
            e.last_seen_ms = now;
            return;
        }
    }
    Entry e;
    e.name = a.usn;
    e.ip   = a.ip;
    e.last_seen_ms = now;
    _entries.push_back(std::move(e));
    Serial.printf("[ScaleDisc] New scale: %s @ %s\n", a.usn.c_str(), a.ip.toString().c_str());
}
