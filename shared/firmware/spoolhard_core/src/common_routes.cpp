#include "spoolhard/common_routes.h"
#include "spoolhard/auth.h"
#include "spoolhard/deferred_reboot.h"
#include "spoolhard/ring_log.h"
#include "spoolhard/psram_json_alloc.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace SpoolhardCommonRoutes {

void registerRestart(AsyncWebServer& server) {
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!SpoolhardAuth::requireAuth(req)) return;
        req->send(200, "application/json", "{\"ok\":true}");
        // send() only queues the response; rebooting from a detached
        // task lets it actually flush (see deferred_reboot.h).
        spoolhardDeferredReboot();
    });
}

void registerLogs(AsyncWebServer& server) {
    // Exact matcher, not the default BackwardCompatible one: that also
    // matches any path under "/api/logs/", and registration order makes
    // this shared route win — it silently shadowed the console's
    // /api/logs/current (SD log) from the esp32async upgrade until
    // 0.12.10. Sub-paths must reach their own product-level handlers.
    server.on(AsyncURIMatcher::exact("/api/logs"), HTTP_GET, [](AsyncWebServerRequest* req) {
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

void registerHeap(AsyncWebServer& server) {
    server.on("/api/heap", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!SpoolhardAuth::requireAuth(req)) return;
        JsonDocument doc(&g_psramJsonAlloc);

        auto fillCaps = [&](const char* key, uint32_t caps) {
            multi_heap_info_t info;
            heap_caps_get_info(&info, caps);
            JsonObject o = doc[key].to<JsonObject>();
            o["free"]          = (uint32_t)info.total_free_bytes;
            o["min_free"]      = (uint32_t)info.minimum_free_bytes;
            o["largest_block"] = (uint32_t)info.largest_free_block;
            o["allocated"]     = (uint32_t)info.total_allocated_bytes;
        };
        fillCaps("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        fillCaps("psram",    MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);

#if (configUSE_TRACE_FACILITY == 1)
        // Per-task stack high-water marks — the data that says which
        // stacks are oversized. Snapshot into a plain array first;
        // uxTaskGetSystemState runs with the scheduler suspended, so
        // keep the JSON work outside it.
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t* st = (TaskStatus_t*)heap_caps_malloc(
            n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!st) st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
        if (st) {
            n = uxTaskGetSystemState(st, n, nullptr);
            JsonArray tasks = doc["tasks"].to<JsonArray>();
            for (UBaseType_t i = 0; i < n; ++i) {
                JsonObject t = tasks.add<JsonObject>();
                t["name"]           = st[i].pcTaskName;
                t["prio"]           = (uint32_t)st[i].uxCurrentPriority;
                // words → bytes; minimum stack headroom ever observed
                t["stack_free_min"] = (uint32_t)st[i].usStackHighWaterMark
                                          * sizeof(StackType_t);
            }
            free(st);
        }
#endif
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
}

}  // namespace SpoolhardCommonRoutes
