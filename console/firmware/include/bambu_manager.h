#pragma once
#include <vector>
#include <memory>
#include <IPAddress.h>
#include "bambu_printer.h"

/**
 * Owns up to BAMBU_MAX_PRINTERS BambuPrinter instances, synced with
 * g_printers_cfg. Call `begin()` once, then `update()` every loop tick.
 * Call `reload()` after the web UI mutates g_printers_cfg.
 */
class BambuManager {
public:
    void begin();
    void update();
    void reload();    // rebuild printer list from g_printers_cfg

    // Force a fresh TLS+MQTT connect attempt on every configured printer.
    // Wired to the Refresh affordances on the LCD and the web UI.
    void reconnectAll();

    // Called from the BambuDiscovery `OnSeen` callback. If the announced
    // serial matches a configured printer and its IP differs from what we
    // stored, persist the new IP and force a reconnect — covers the
    // "printer powered on with a fresh DHCP lease" case without manual
    // edit. Also nudges a reconnect when the IP is unchanged but the
    // current link state is Failed/Disconnected, so a printer that just
    // came back up gets reattached on the very next announcement instead
    // of waiting for the 5 s gate plus the slow TLS timeout.
    void onAnnounce(const String& serial, const IPAddress& ip);

    // Read-only access for the web server / UI.
    const std::vector<std::unique_ptr<BambuPrinter>>& printers() const { return _printers; }
    const BambuPrinter* find(const String& serial) const;
    BambuPrinter*       find(const String& serial);

private:
    std::vector<std::unique_ptr<BambuPrinter>> _printers;
};

extern BambuManager g_bambu;
