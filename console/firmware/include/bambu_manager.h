#pragma once
#include <vector>
#include <memory>
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

    // Read-only access for the web server / UI.
    const std::vector<std::unique_ptr<BambuPrinter>>& printers() const { return _printers; }
    const BambuPrinter* find(const String& serial) const;
    BambuPrinter*       find(const String& serial);

private:
    std::vector<std::unique_ptr<BambuPrinter>> _printers;
};

extern BambuManager g_bambu;
