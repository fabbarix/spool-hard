#include "spoolhard/common_routes.h"
#include "spoolhard/auth.h"
#include "spoolhard/ring_log.h"
#include "spoolhard/psram_json_alloc.h"
#include <ArduinoJson.h>

namespace SpoolhardCommonRoutes {

void registerRestart(AsyncWebServer& server) {
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!SpoolhardAuth::requireAuth(req)) return;
        req->send(200, "application/json", "{\"ok\":true}");
        // Drift the reboot a touch beyond the response flush. This is
        // unfortunately a synchronous delay() inside the AsyncTCP task,
        // but the alternative (scheduling a reboot from a one-shot timer)
        // is more code for the same observable behaviour.
        delay(500);
        ESP.restart();
    });
}

void registerLogs(AsyncWebServer& server) {
    server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!SpoolhardAuth::requireAuth(req)) return;
        uint32_t since = 0;
        if (req->hasParam("since")) {
            since = (uint32_t)req->getParam("since")->value().toInt();
        }
        auto rows = RingLog::snapshot(since, 200);
        JsonDocument doc(&g_psramJsonAlloc);
        doc["head"]  = RingLog::headSeq();
        doc["since"] = since;
        JsonArray arr = doc["lines"].to<JsonArray>();
        for (auto& e : rows) {
            JsonObject o = arr.add<JsonObject>();
            o["seq"]    = e.seq;
            o["t_ms"]   = e.millis_at;
            o["text"]   = e.line;
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
}

}  // namespace SpoolhardCommonRoutes
