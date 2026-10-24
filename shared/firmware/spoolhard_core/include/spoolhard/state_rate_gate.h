#pragma once
#include <Arduino.h>
#include <string.h>

// Per-resource min-interval gate for WebSocket `state.*` pushes.
//
// Each product owns its own table (which resources are high-rate differs);
// the lookup/gate logic is shared. Resources absent from the table bypass
// the gate. Usage:
//
//   static StateRateLimit s_rates[] = { {"ota", 5000, 0}, ... };
//   static StateRateGate  s_gate(s_rates, sizeof(s_rates)/sizeof(s_rates[0]));
//   if (!s_gate.pass("ota")) return;
struct StateRateLimit { const char* key; uint32_t min_interval_ms; uint32_t last_ms; };

class StateRateGate {
public:
    StateRateGate(StateRateLimit* table, size_t n) : _t(table), _n(n) {}

    // Consuming check: true = send now (and stamps the time), false = drop.
    bool pass(const char* resource) {
        uint32_t now = millis();
        for (size_t i = 0; i < _n; ++i) {
            StateRateLimit& r = _t[i];
            if (strcmp(r.key, resource) != 0) continue;
            if (r.last_ms != 0 && (now - r.last_ms) < r.min_interval_ms) return false;
            r.last_ms = now;
            return true;
        }
        return true;   // not in table = no gate
    }

    // Non-consuming peek: would pass() let this resource through right now?
    bool due(const char* resource) const {
        uint32_t now = millis();
        for (size_t i = 0; i < _n; ++i) {
            const StateRateLimit& r = _t[i];
            if (strcmp(r.key, resource) != 0) continue;
            return r.last_ms == 0 || (now - r.last_ms) >= r.min_interval_ms;
        }
        return true;
    }

private:
    StateRateLimit* _t;
    size_t _n;
};
