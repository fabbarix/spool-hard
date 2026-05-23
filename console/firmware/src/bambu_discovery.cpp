#include "bambu_discovery.h"
#include "config.h"
#include "ssdp_hub.h"
#include "spoolhard/serial_mirror.h"

BambuDiscovery g_bambu_discovery;

void BambuDiscovery::begin() {
    auto onAnnounce = [this](const SsdpListener::Announce& a) {
        // Only Bambu devices — filter by URN substring so minor firmware
        // variations (capitalisation, version suffix) still match. The same
        // filter on the multicast :1990/:1900 listeners harmlessly drops
        // scale NOTIFY and Chromecast/UPnP chatter.
        String urn = a.urn; urn.toLowerCase();
        if (urn.indexOf(BAMBU_SSDP_URN_TAG) < 0) return;
        _onAnnounce(a, "");
    };
    g_ssdp_1990.subscribe(onAnnounce);        // legacy/back-compat (unused by current FW)
    g_ssdp_1900.subscribe(onAnnounce);        // legacy/back-compat (unused by current FW)
    g_ssdp_bambu_2021.subscribe(onAnnounce);  // the real Bambu broadcast channel
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

    // Prefer the per-frame Bambu fields over the legacy `model` parameter
    // (which is always "" for the current call sites). The NOTIFY frame
    // carries DevName.bambu.com (e.g. "Bambozzo") and DevModel.bambu.com
    // (e.g. "O1S") that the frontend uses for auto-fill labels.
    const String& effectiveModel = a.dev_model.length() ? a.dev_model : model;

    uint32_t now = millis();
    for (auto& e : _entries) {
        if (e.serial == a.usn) {
            e.ip = a.ip;
            e.last_seen_ms = now;
            if (effectiveModel.length()) e.model = effectiveModel;
            if (a.dev_name.length())     e.name  = a.dev_name;
            if (_onSeen) _onSeen(e);
            return;
        }
    }
    Entry e;
    e.serial = a.usn;
    e.ip     = a.ip;
    e.model  = effectiveModel;
    e.name   = a.dev_name;
    e.last_seen_ms = now;
    _entries.push_back(e);
    Serial.printf("[BambuDisc] New printer: %s '%s' (%s) @ %s\n",
                  a.usn.c_str(),
                  e.name.length()  ? e.name.c_str()  : "?",
                  e.model.length() ? e.model.c_str() : "?",
                  a.ip.toString().c_str());
    if (_onSeen) _onSeen(_entries.back());
}
