#pragma once
#include <ESPAsyncWebServer.h>

// Identical HTTP routes that both the console and scale firmwares
// expose. Lifted to the shared lib so any change (auth shape, body
// format, error semantics) lands on both products in one edit.
//
// firmware-info stays per-product because it carries product-specific
// optional fields (console adds printer count + SD stats; scale adds
// calibration status). Auth handlers live in spoolhard/auth.h.
namespace SpoolhardCommonRoutes {

// POST /api/restart — auth-gated. Sends 200 then schedules a reboot
// 500ms later so the response can flush before the radio drops.
void registerRestart(AsyncWebServer& server);

// GET /api/logs?since=<seq> — auth-gated. Snapshots up to 200 entries
// from the RingLog beyond the given sequence number.
//
// NB: `/api/logs/current` is intentionally NOT registered here — the
// console firmware overloads that path with SD-persisted log retrieval
// (see CrashLogger). Each product registers its own handler for that
// route.
void registerLogs(AsyncWebServer& server);

// Convenience: register all of the above in one call.
inline void registerAll(AsyncWebServer& server) {
    registerRestart(server);
    registerLogs(server);
}

}  // namespace SpoolhardCommonRoutes
