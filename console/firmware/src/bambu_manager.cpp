#include "bambu_manager.h"
#include "printer_config.h"
#include <WiFi.h>

BambuManager g_bambu;

void BambuManager::begin() {
    g_printers_cfg.load();
    reload();
}

void BambuManager::reload() {
    // Build desired set.
    const auto& desired = g_printers_cfg.list();

    // Drop printers that are no longer in config.
    _printers.erase(
        std::remove_if(_printers.begin(), _printers.end(),
            [&](const std::unique_ptr<BambuPrinter>& p) {
                for (const auto& d : desired) {
                    if (d.serial == p->config().serial) return false;
                }
                return true;  // not in desired — drop
            }),
        _printers.end()
    );

    // Add or update to match config.
    for (const auto& cfg : desired) {
        bool found = false;
        for (auto& existing : _printers) {
            if (existing->config().serial == cfg.serial) {
                existing->updateConfig(cfg);
                found = true;
                break;
            }
        }
        if (!found) {
            _printers.emplace_back(std::make_unique<BambuPrinter>(cfg));
            Serial.printf("[Bambu] Added printer %s (%s)\n",
                          cfg.name.c_str(), cfg.serial.c_str());
        }
    }
}

void BambuManager::update() {
    if (WiFi.status() != WL_CONNECTED) return;
    for (auto& p : _printers) p->loop();
}

const BambuPrinter* BambuManager::find(const String& serial) const {
    for (const auto& p : _printers) {
        if (p->config().serial == serial) return p.get();
    }
    return nullptr;
}

BambuPrinter* BambuManager::find(const String& serial) {
    for (auto& p : _printers) {
        if (p->config().serial == serial) return p.get();
    }
    return nullptr;
}

void BambuManager::reconnectAll() {
    for (auto& p : _printers) p->forceReconnect();
}

void BambuManager::onAnnounce(const String& serial, const IPAddress& ip) {
    if (serial.isEmpty() || (uint32_t)ip == 0) return;
    BambuPrinter* p = find(serial);
    if (!p) return;
    String newIp = ip.toString();
    if (p->config().ip != newIp) {
        Serial.printf("[Bambu] %s IP changed %s -> %s — updating + reconnecting\n",
                      serial.c_str(), p->config().ip.c_str(), newIp.c_str());
        PrinterConfig cfg = p->config();
        cfg.ip = newIp;
        g_printers_cfg.upsert(cfg);
        g_printers_cfg.save();
        p->updateConfig(cfg);   // already drops MQTT and resets the gate
        return;
    }
    // IP unchanged but we're not currently linked — nudge the retry gate
    // so we don't sit out the rest of a 30 s TLS timeout cycle when the
    // printer is clearly back on the air.
    auto link = p->state().link;
    if (link == BambuLinkState::Failed || link == BambuLinkState::Disconnected) {
        p->forceReconnect();
    }
}
