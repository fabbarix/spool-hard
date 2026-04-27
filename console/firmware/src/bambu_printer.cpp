#include "bambu_printer.h"
#include "store.h"
#include "printer_ftp.h"
#include "zip_reader.h"
#include "gcode_analyzer.h"
#include "scale_link.h"
#include "pending_ams.h"
#include "printer_config.h"
#include "web_server.h"
#include "ui/ui.h"
#include "ring_log.h"
#include <WiFiClientSecure.h>
// Raw-deflate inflater bundled in ROM. Bambu's slicer started shipping
// some 3MFs with the gcode entry deflate-compressed (method=8) instead
// of stored, so analyseRemote needs to inflate on the fly. We use the
// low-level tinfl_decompress call (rather than tinfl_decompress_mem_to_
// callback, which requires the entire compressed input to be in memory)
// so we can pipe the FTP retrieveRange callback's chunks straight into
// the inflater and keep peak memory bounded by the 32 KiB sliding dict.
#include <rom/miniz.h>
#include <PubSubClient.h>
#include <IPAddress.h>
#include <esp_heap_caps.h>   // PSRAM-capable allocator for the analyzer
#include <new>               // placement new
#include <lvgl.h>   // LV_SYMBOL_* — used for the LCD ams-status label

extern ScaleLink g_scale;   // defined in main.cpp

extern SpoolStore g_store;   // defined in main.cpp
// g_printers_cfg is already declared in printer_config.h.

// Static hook invoked once per successful PendingAms claim.
BambuPrinter::SpoolAssignedCb BambuPrinter::s_onSpoolAssigned;

// ── PSRAM-backed task spawning ──────────────────────────────────────
//
// The analyse + FTP-debug tasks each want a 16 KiB FreeRTOS stack from
// internal DRAM. Mid-print the contiguous-internal-DRAM budget gets
// tight (Bambu pushall payloads, mbedtls handshake contexts for MQTT
// / FTPS, LVGL frames…) and `xTaskCreatePinnedToCore` returns
// errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY. The kicker: the analyse-task
// itself is mostly TLS + I/O wait, so it doesn't need the speed of
// internal RAM — putting its stack in PSRAM is fine. PSRAM has ~1.9 MB
// free, vs ~40 KiB internal DRAM mid-print.
//
// Strategy: try internal first (fast path; preserves PSRAM for big
// allocs that actually need it), fall back to a static-PSRAM stack on
// failure. One static TCB+stack pair PER task type; concurrency
// inside a type is already gated (analyse: _analysisInProgress;
// ftp-debug: _ftpDebugBusy) so reuse is safe.
//
// For the static-task path, we lazy-allocate the PSRAM stack on first
// fallback and keep it allocated for the lifetime of the firmware —
// next spawn reuses the same buffer. xTaskCreateStatic doesn't auto-
// free memory; reusing the buffer avoids a leak.
namespace {

constexpr size_t TASK_STACK_BYTES = 16384;
constexpr size_t TASK_STACK_WORDS = TASK_STACK_BYTES / sizeof(StackType_t);

// Three reusable PSRAM stacks — one each for the analyse, FTP-debug,
// and MQTT-connect task slots. They're orthogonal (each can run
// concurrently with the others). The `busy` flag prevents reuse mid-
// task (e.g. two BambuPrinter instances both falling back to PSRAM
// for connect at the same time): the second caller gets a refusal
// from spawnPSRAMFallbackTask and the standard 5 s retry covers it.
struct PSRAMStackSlot {
    StaticTask_t  tcb;
    StackType_t*  buf = nullptr;
    volatile bool busy = false;   // true while a task owns the buffer
};
PSRAMStackSlot s_analyseSlot;
PSRAMStackSlot s_ftpDebugSlot;
PSRAMStackSlot s_connectSlot;

// Spawn a one-shot task with internal-stack-then-PSRAM-stack fallback.
// Caller's `fn` is responsible for vTaskDelete(NULL) at the end (same
// contract as our existing trampolines) AND, if it ran on the PSRAM
// slot, must clear `slot.busy = false` before vTaskDelete so the
// buffer can be reused. Returns true on success.
bool spawnPSRAMFallbackTask(TaskFunction_t fn, void* arg, const char* name,
                            BaseType_t core_id, UBaseType_t priority,
                            PSRAMStackSlot& slot) {
    // Try internal heap first — fast path, keeps PSRAM free for big
    // analysis allocs (gcode-analyzer's mmAtPct table, etc).
    BaseType_t rc = xTaskCreatePinnedToCore(fn, name, TASK_STACK_BYTES,
                                            arg, priority, nullptr, core_id);
    if (rc == pdPASS) return true;
    Serial.printf("[Task %s] internal-heap stack alloc failed; trying PSRAM\n", name);
    if (slot.busy) {
        Serial.printf("[Task %s] PSRAM slot already in use — caller will retry\n", name);
        return false;
    }
    if (!slot.buf) {
        slot.buf = (StackType_t*)heap_caps_malloc(
            TASK_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!slot.buf) {
        Serial.printf("[Task %s] PSRAM stack alloc also failed\n", name);
        return false;
    }
    slot.busy = true;
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        fn, name, TASK_STACK_WORDS, arg, priority,
        slot.buf, &slot.tcb, core_id);
    if (!h) {
        slot.busy = false;
        Serial.printf("[Task %s] xTaskCreateStaticPinnedToCore returned null\n", name);
        return false;
    }
    Serial.printf("[Task %s] running on PSRAM stack\n", name);
    return true;
}

}  // namespace

namespace {
// Fallback nozzle temps per material. Used when a SpoolRecord has no
// user-entered nozzle_temp_min/max — the printer needs non-zero values or it
// just rejects the ams_filament_setting command. Values are the conservative
// defaults Bambu Studio ships for each filament category.
struct TempDefault { const char* material; uint16_t tmin; uint16_t tmax; };
constexpr TempDefault kTempDefaults[] = {
    {"PLA",     190, 230},
    {"PETG",    220, 260},
    {"ABS",     240, 270},
    {"ASA",     240, 270},
    {"TPU",     210, 240},
    {"PA",      260, 300},
    {"PA-CF",   270, 300},
    {"PC",      250, 280},
    {"PET-CF",  260, 290},
};
void resolveTemps(const String& material, int32_t user_min, int32_t user_max,
                  uint32_t& tmin, uint32_t& tmax) {
    // User-entered temps win.
    uint16_t d_min = 190, d_max = 230;  // PLA fallback
    for (const auto& td : kTempDefaults) {
        if (material.equalsIgnoreCase(td.material)) {
            d_min = td.tmin; d_max = td.tmax; break;
        }
    }
    tmin = (user_min > 0) ? (uint32_t)user_min : d_min;
    tmax = (user_max > 0) ? (uint32_t)user_max : d_max;
}
}  // namespace

BambuPrinter::BambuPrinter(const PrinterConfig& cfg) : _cfg(cfg) {
    _wifi = new WiFiClientSecure();
    _wifi->setInsecure();   // Bambu printers use self-signed certs in LAN mode.
    _mqtt = new PubSubClient(*_wifi);
    _mqtt->setBufferSize(MQTT_MAX_PACKET_SIZE);
    _mqtt->setSocketTimeout(5);
    _mqtt->setCallback([this](char* t, uint8_t* p, unsigned int l) {
        _onMessage(t, p, l);
    });
}

BambuPrinter::~BambuPrinter() {
    // Wait out any in-flight connect task before tearing _mqtt/_wifi down —
    // the task is still using them. Worst-case wait is the WiFiClientSecure
    // connect timeout (~30 s); destructors only run on config change so this
    // is rare and not in a hot path.
    while (_connectTaskState == ConnectTaskState::Pending) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (_mqtt) { _mqtt->disconnect(); delete _mqtt; }
    if (_wifi) { delete _wifi; }
}

void BambuPrinter::updateConfig(const PrinterConfig& cfg) {
    bool reconnect = (cfg.ip != _cfg.ip) || (cfg.access_code != _cfg.access_code)
                   || (cfg.serial != _cfg.serial);
    _cfg = cfg;
    // Skip the synchronous disconnect while a background connect task owns
    // _mqtt — the next _ensureConnected() tick after the task drains will
    // see _mqtt->connected() against the new config and reconnect cleanly.
    if (reconnect && _connectTaskState != ConnectTaskState::Pending &&
        _mqtt && _mqtt->connected()) {
        _mqtt->disconnect();
    }
    if (reconnect) {
        _lastConnectAttemptMs = 0;   // bypass the 5 s gate on next loop()
    }
}

void BambuPrinter::forceReconnect() {
    // Don't fight a pending background connect — it owns _mqtt. Just reset
    // the gate so the next harvest tick promptly retries on failure.
    _lastConnectAttemptMs = 0;
    if (_connectTaskState == ConnectTaskState::Pending) return;
    if (_mqtt && _mqtt->connected()) {
        _mqtt->disconnect();
        _state.link = BambuLinkState::Disconnected;
    }
    Serial.printf("[Bambu %s] forceReconnect requested\n", _cfg.serial.c_str());
}

// Runs on its own pinned-to-core-0 task; nobody else touches _mqtt/_wifi
// until _connectTaskState flips to Done. mbedtls' TLS handshake needs both
// significant heap (~30 KB) AND stack (~10 KB) — task stack sized to suit.
void BambuPrinter::_connectTrampoline(void* arg) {
    auto* self = static_cast<BambuPrinter*>(arg);
    String clientId = "SpoolHard-" + self->_cfg.serial;
    bool ok = self->_mqtt->connect(clientId.c_str(), "bblp",
                                   self->_cfg.access_code.c_str());
    self->_connectTaskResult = ok;
    self->_connectTaskState  = ConnectTaskState::Done;
    self->_connectTask       = nullptr;
    // Release the PSRAM stack slot if we ran from there. Safe to do
    // unconditionally — clearing a flag that's already false is a no-
    // op, and we don't track which task path ran (internal-stack
    // tasks just leave the flag false the whole time).
    s_connectSlot.busy = false;
    vTaskDelete(NULL);
}

void BambuPrinter::_ensureConnected() {
    // Harvest a completed background connect — runs in the main loop, so
    // it's safe to do post-connect setup (subscribe, publish) here.
    if (_connectTaskState == ConnectTaskState::Done) {
        _connectTaskState = ConnectTaskState::Idle;
        if (_connectTaskResult && _mqtt->connected()) {
            Serial.printf("[Bambu %s] Connected\n", _cfg.serial.c_str());
            _state.link = BambuLinkState::Connected;
            _state.error_message = "";
            String topic = "device/" + _cfg.serial + "/report";
            _mqtt->subscribe(topic.c_str());
            requestFullStatus();
        } else {
            _state.link = BambuLinkState::Failed;
            char err[32];
            snprintf(err, sizeof(err), "mqtt rc=%d", _mqtt->state());
            _state.error_message = err;
            Serial.printf("[Bambu %s] Connect failed: %s\n", _cfg.serial.c_str(), err);
        }
        return;
    }
    if (_connectTaskState == ConnectTaskState::Pending) return;
    if (_mqtt->connected()) return;

    uint32_t now = millis();
    if (now - _lastConnectAttemptMs < 5000) return;
    _lastConnectAttemptMs = now;

    if (_cfg.ip.isEmpty() || _cfg.serial.isEmpty() || _cfg.access_code.isEmpty()) {
        _state.link = BambuLinkState::Failed;
        _state.error_message = "missing ip/serial/access_code";
        return;
    }

    _state.link = BambuLinkState::Connecting;
    _mqtt->setServer(_cfg.ip.c_str(), BAMBU_MQTT_PORT);

    // Hand the blocking PubSubClient::connect() to a one-shot task.
    // Pinned to core 0 with low priority so it can't preempt the main
    // loop on core 1; main loop polls _connectTaskState each tick to
    // harvest. spawnPSRAMFallbackTask tries internal heap first
    // (faster, keeps PSRAM free for big allocs) and falls back to
    // a static PSRAM stack when internal DRAM is fragmented — typical
    // mid-print, when a Reconnect tap from the LCD would otherwise
    // silently fail to spawn the connect task and look broken.
    _connectTaskResult = false;
    _connectTaskState  = ConnectTaskState::Pending;
    if (!spawnPSRAMFallbackTask(_connectTrampoline, this, "bambu_conn",
                                /*core*/0, /*priority*/1, s_connectSlot)) {
        // Couldn't spawn even with PSRAM fallback (PSRAM exhausted or
        // slot already in use by another printer's connect attempt).
        // Drop back to Idle so the next 5 s retry tries again.
        _connectTaskState = ConnectTaskState::Idle;
        _state.link       = BambuLinkState::Failed;
        _state.error_message = "task spawn failed";
        Serial.printf("[Bambu %s] connect task spawn failed (internal+PSRAM)\n",
                      _cfg.serial.c_str());
    }
}

void BambuPrinter::requestFullStatus() {
    if (!_mqtt || !_mqtt->connected()) return;
    String topic = "device/" + _cfg.serial + "/request";
    const char* payload =
        "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
    _mqtt->publish(topic.c_str(), payload);
}

void BambuPrinter::loop() {
    _ensureConnected();
    // Skip _mqtt->loop() while the connect task owns _mqtt; PubSubClient
    // and WiFiClientSecure are not safe to read from the main loop while
    // mbedtls is in the middle of a handshake on the task.
    if (_mqtt && _connectTaskState != ConnectTaskState::Pending) _mqtt->loop();

    // Periodic pushall in case the printer only broadcasts deltas we missed.
    if (_state.link == BambuLinkState::Connected && millis() - _lastPollMs > 30000) {
        _lastPollMs = millis();
        requestFullStatus();
    }

    // Retry a previously-failed analyseRemote when the backoff elapses —
    // body lives near the task trampoline further down in this file so
    // it can reach the static types.
    _maybeRetryAnalysis();
}

void BambuPrinter::_onMessage(const char* /*topic*/, uint8_t* payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[Bambu %s] parse error: %s (len=%u)\n",
                      _cfg.serial.c_str(), err.c_str(), length);
        return;
    }
    _parseReport(doc);
    _state.last_report_ms = millis();
}

void BambuPrinter::_parseReport(const JsonDocument& doc) {
    // Bambu publishes {"print": {...}} or {"info": {...}}; we only care about print.
    JsonObjectConst print = doc["print"];
    if (!print) return;

    String prevGcode = _state.gcode_state;
    if (print["gcode_state"].is<const char*>())  _state.gcode_state  = print["gcode_state"].as<String>();
    if (print["subtask_name"].is<const char*>()) _state.subtask_name = print["subtask_name"].as<String>();
    if (print["gcode_file"].is<const char*>())   _state.gcode_file   = print["gcode_file"].as<String>();
    if (_state.gcode_state != prevGcode) {
        _handleGcodeStateTransition(prevGcode, _state.gcode_state);
    }
    if (print["mc_percent"].is<int>()) {
        _state.progress_pct = print["mc_percent"];
        // Live-commit incremental consumption as the printer progresses. A
        // no-op unless we're in an active gcode_state with a valid 3mf
        // analysis and progress has crossed a new 5% boundary.
        _commitIncrementalConsumption();
    }
    if (print["layer_num"].is<int>())            _state.layer_num    = print["layer_num"];
    if (print["total_layer_num"].is<int>())      _state.total_layers = print["total_layer_num"];
    if (print["bed_temper"].is<float>())         _state.bed_temp     = print["bed_temper"];
    if (print["nozzle_temper"].is<float>())      _state.nozzle_temp  = print["nozzle_temper"];

    // Nozzle inventory (X1/P1 typically one, H2D two). Needed to key K-value
    // storage correctly since Bambu's pressure-advance is per-nozzle-diameter.
    JsonObjectConst device = print["device"];
    if (device) {
        JsonObjectConst nozzleInfo = device["nozzle"];
        if (nozzleInfo) {
            JsonArrayConst info = nozzleInfo["info"];
            _state.nozzle_count = 0;
            for (JsonVariantConst n : info) {
                if (_state.nozzle_count >= 2) break;
                _state.nozzle_diameters[_state.nozzle_count] = n["diameter"] | 0.f;
                ++_state.nozzle_count;
            }
        }
    }

    JsonObjectConst ams = print["ams"];
    if (ams) {
        // tray_now / tray_tar are stringified ints in Bambu's wire format.
        if (ams["tray_now"].is<const char*>())
            _state.active_tray = atoi(ams["tray_now"].as<const char*>());

        _parseAms(ams, _state);
        _persistKValues();
        _persistTrayInfoIdx();
        _autoRestoreKValues();

        // Optional raw-AMS broadcast to the debug WebSocket — user-toggled
        // via Config → Debug → "Log AMS messages". Off by default because
        // each message is 1–5 KB and AMS reports land every few seconds.
        if (g_web._logAmsRaw) {
            JsonDocument payload;
            payload["serial"] = _cfg.serial;
            payload["ams"]    = ams;
            if (print["vt_tray"].is<JsonObjectConst>()) {
                payload["vt_tray"] = print["vt_tray"];
            }
            g_web.broadcastDebug("ams_raw", payload);
        }
    }

    // External tray (`vt_tray`) — single-extruder printers use this for a
    // regular spool holder rather than an AMS slot. Same schema as an AMS
    // tray, lives at the top-level `print.vt_tray`.
    JsonObjectConst vt = print["vt_tray"];
    if (vt) {
        AmsTray& tr = _state.vt_tray;
        tr.id            = 254;                    // yanshay's convention for external
        tr.tray_type     = vt["tray_type"]     | "";
        tr.tray_color    = vt["tray_color"]    | "";
        tr.tray_info_idx = vt["tray_info_idx"] | "";
        tr.tag_uid       = vt["tag_uid"]       | "";
        if (vt["nozzle_temp_min"].is<const char*>()) tr.nozzle_min_c = atoi(vt["nozzle_temp_min"].as<const char*>());
        if (vt["nozzle_temp_max"].is<const char*>()) tr.nozzle_max_c = atoi(vt["nozzle_temp_max"].as<const char*>());
        if (vt["remain"].is<int>())   tr.remain_pct = vt["remain"];
        if (vt["k"].is<float>())      tr.k          = vt["k"];
        if (vt["cali_idx"].is<int>()) tr.cali_idx   = vt["cali_idx"];

        // Same populated→empty unload detection as AMS slots — drop the
        // override before resolution so a removed spool doesn't keep the
        // tray "pinned" to the old id.
        String vt_sig = tr.tray_type + "|" + tr.tray_info_idx + "|" + tr.tray_color;
        bool vt_now_populated = tr.tray_type.length() ||
                                tr.tray_info_idx.length() ||
                                tr.tray_color.length();
        if (_vtSignatureSeen) {
            bool was_empty = _lastVtSignature.isEmpty() || _lastVtSignature == "||";
            if (!vt_now_populated && !was_empty) {
                Serial.printf("[Bambu %s] external tray physically unloaded\n",
                              _cfg.serial.c_str());
                const PrinterConfig* cfg_ro = g_printers_cfg.find(_cfg.serial);
                if (cfg_ro && cfg_ro->findAmsOverride(254, 0).length()) {
                    PrinterConfig cfg_copy = *cfg_ro;
                    cfg_copy.setAmsOverride(254, 0, "");
                    g_printers_cfg.upsert(cfg_copy);
                    g_printers_cfg.save();
                    _cfg = cfg_copy;
                }
            }
        }
        _lastVtSignature = vt_sig;
        _vtSignatureSeen = true;

        tr.mapped_spool_id     = "";
        tr.mapped_via_override = false;
        String override_id = _cfg.findAmsOverride(/*ams_unit*/ 254, /*slot*/ 0);
        if (override_id.length()) {
            SpoolRecord rec;
            if (g_store.findById(override_id, rec)) {
                tr.mapped_spool_id     = override_id;
                tr.mapped_via_override = true;
            }
        }
        if (tr.mapped_spool_id.isEmpty() && tr.tag_uid.length()) {
            SpoolRecord rec;
            if (g_store.findByTagId(tr.tag_uid, rec)) tr.mapped_spool_id = rec.id;
        }
        _syncSpoolToPrinter(/*ams_unit*/ 254, /*slot*/ 0, tr);
        _state.has_vt_tray = true;
    }
}

void BambuPrinter::_autoRestoreKValues() {
    // Edge-triggered: on the tick where a tray's tag_uid changes (spool
    // inserted or swapped), look up our stored K for (this printer, this
    // nozzle diameter, extruder 0) and push it to the printer if it differs
    // from what the printer is currently reporting. Gated on the printer's
    // auto_restore_k flag.
    if (!_cfg.auto_restore_k) return;
    if (_state.nozzle_count == 0) return;
    const float nozzle = _state.nozzle_diameters[0];
    const int   extruder = 0;

    for (int u = 0; u < _state.ams_count && u < 4; ++u) {
        for (int t = 0; t < 4; ++t) {
            const AmsTray& tr = _state.ams[u].trays[t];
            String now_uid = tr.tag_uid;
            String& last_uid = _lastTagUid[u][t];
            if (now_uid == last_uid) continue;
            last_uid = now_uid;

            // Empty → empty transitions never reach here (caught by equality).
            // Empty → something (or swap) is the trigger.
            if (now_uid.isEmpty() || tr.mapped_spool_id.isEmpty()) continue;

            SpoolRecord rec;
            if (!g_store.findById(tr.mapped_spool_id, rec)) continue;
            // Read the ext.k_values[] for a matching (printer, nozzle, extruder).
            JsonDocument ext;
            if (rec.ext_json.length() && deserializeJson(ext, rec.ext_json)) continue;
            JsonArrayConst arr = ext["k_values"].as<JsonArrayConst>();
            if (!arr) continue;
            float stored_k = 0.f;
            for (JsonVariantConst e : arr) {
                if (e["printer"] != _cfg.serial.c_str()) continue;
                float n = e["nozzle"] | 0.f;
                if (fabsf(n - nozzle) > 0.01f) continue;
                if ((e["extruder"] | 0) != extruder) continue;
                stored_k = e["k"] | 0.f;
                break;
            }
            if (stored_k <= 0.f) continue;
            if (fabsf(stored_k - tr.k) < 0.0005f) continue;  // already matches

            _pushKRestore(u, t, tr, nozzle, stored_k);
        }
    }
}

void BambuPrinter::_pushKRestore(int ams_id, int slot_id, const AmsTray& tr, float nozzle, float k) {
    if (!_mqtt || !_mqtt->connected()) return;
    // Bambu wire format mirrors yanshay/SpoolEase bambu_api::ExtrusionCaliSet.
    char nozzleStr[8]; snprintf(nozzleStr, sizeof(nozzleStr), "%.1f", nozzle);
    char kStr[16];      snprintf(kStr,      sizeof(kStr),      "%.3f", k);

    JsonDocument doc;
    JsonObject print = doc["print"].to<JsonObject>();
    print["command"]         = "extrusion_cali_set";
    print["nozzle_diameter"] = nozzleStr;
    print["sequence_id"]     = "1";
    JsonArray filaments = print["filaments"].to<JsonArray>();
    JsonObject f = filaments.add<JsonObject>();
    f["ams_id"]          = ams_id;
    f["extruder_id"]     = 0;
    f["filament_id"]     = tr.tray_info_idx;
    f["k_value"]         = kStr;
    f["n_coef"]          = "0.000000";
    f["name"]            = "SpoolHard";
    f["nozzle_diameter"] = nozzleStr;
    f["nozzle_id"]       = "";
    f["setting_id"]      = "";
    f["slot_id"]         = slot_id;
    f["tray_id"]         = -1;

    String payload;
    serializeJson(doc, payload);

    String topic = "device/" + _cfg.serial + "/request";
    _mqtt->publish(topic.c_str(), payload.c_str());
    Serial.printf("[Bambu %s] auto-restore K=%.3f to AMS %d/%d (nozzle %.1f)\n",
                  _cfg.serial.c_str(), k, ams_id, slot_id, nozzle);
}

void BambuPrinter::_persistTrayInfoIdx() {
    // For every AMS slot mapped to a spool, if Bambu reports a non-empty
    // tray_info_idx that differs from the spool's stored slicer_filament,
    // adopt the printer's value. The printer treats tray_info_idx as the
    // canonical "what filament is in this slot" identifier — when the
    // user picks a filament from the touchscreen panel after loading a
    // SpoolHard tag, that pick lands here. Without this sync, the spool
    // record would silently disagree with the printer forever.
    //
    // Only writes when the value actually changes — store.upsert is a
    // full-line rewrite on JSONL, no point churning the SD card.
    auto syncTray = [&](const AmsTray& tr) {
        if (tr.mapped_spool_id.isEmpty() || tr.tray_info_idx.isEmpty()) return;
        SpoolRecord rec;
        if (!g_store.findById(tr.mapped_spool_id, rec)) return;
        if (rec.slicer_filament == tr.tray_info_idx) return;
        Serial.printf("[Bambu %s] tray_info_idx sync for spool %s: '%s' → '%s'\n",
                      _cfg.serial.c_str(), rec.id.c_str(),
                      rec.slicer_filament.c_str(), tr.tray_info_idx.c_str());
        rec.slicer_filament = tr.tray_info_idx;
        g_store.upsert(rec);
    };

    for (int u = 0; u < _state.ams_count; ++u) {
        for (int t = 0; t < 4; ++t) {
            syncTray(_state.ams[u].trays[t]);
        }
    }
    syncTray(_state.vt_tray);
}

void BambuPrinter::_persistKValues() {
    // Bambu broadcasts the current pressure-advance K in every report. We
    // mirror it into the mapped spool whenever it changes so the value is
    // preserved when the spool moves to another printer or AMS slot. This
    // is the "capture" half of M3's K-value handling; auto-restore (M3.2)
    // will consult the same ext.k_values[] when loading a spool.
    if (_state.nozzle_count == 0) return;  // no nozzle info reported yet
    const float nozzle = _state.nozzle_diameters[0];  // extruder 0 for X1/P1
    const int   extruder = 0;

    for (int u = 0; u < _state.ams_count; ++u) {
        for (int t = 0; t < 4; ++t) {
            const AmsTray& tr = _state.ams[u].trays[t];
            if (tr.mapped_spool_id.isEmpty() || tr.k <= 0.f) continue;

            SpoolRecord rec;
            if (!g_store.findById(tr.mapped_spool_id, rec)) continue;
            if (rec.upsertKValue(_cfg.serial, nozzle, extruder, tr.k, tr.cali_idx)) {
                g_store.upsert(rec);
                Serial.printf("[Bambu %s] saved K=%.3f for spool %s (nozzle %.1f)\n",
                              _cfg.serial.c_str(), tr.k,
                              rec.id.c_str(), nozzle);
            }
        }
    }
}

void BambuPrinter::_parseAms(const JsonObjectConst& amsObj, PrinterState& out) {
    // PrintAms.ams is an array of AMS units, each with 4 trays.
    JsonArrayConst units = amsObj["ams"];
    if (!units) return;

    int u = 0;
    for (JsonVariantConst unit : units) {
        if (u >= 4) break;
        AmsUnit& dst = out.ams[u];
        dst.id       = atoi(unit["id"] | "-1");
        dst.humidity = atoi(unit["humidity"] | "-1");

        JsonArrayConst trays = unit["tray"];
        int t = 0;
        for (JsonVariantConst tray : trays) {
            if (t >= 4) break;
            AmsTray& tr = dst.trays[t];
            tr.id            = atoi(tray["id"] | "-1");
            tr.tray_type     = tray["tray_type"]     | "";
            tr.tray_color    = tray["tray_color"]    | "";
            tr.tray_info_idx = tray["tray_info_idx"] | "";
            tr.tag_uid       = tray["tag_uid"]       | "";
            if (tray["nozzle_temp_min"].is<const char*>()) tr.nozzle_min_c = atoi(tray["nozzle_temp_min"].as<const char*>());
            if (tray["nozzle_temp_max"].is<const char*>()) tr.nozzle_max_c = atoi(tray["nozzle_temp_max"].as<const char*>());
            if (tray["remain"].is<int>())   tr.remain_pct = tray["remain"];
            if (tray["k"].is<float>())      tr.k          = tray["k"];
            if (tray["cali_idx"].is<int>()) tr.cali_idx   = tray["cali_idx"];

            // Physical-transition detection has to run BEFORE the mapping
            // resolution so the unload branch can clear the sticky override.
            // Without this, `mapped_spool_id` would stay pinned to the prior
            // spool via the override, `_syncSpoolToPrinter` would see no
            // change, and the slot would masquerade as loaded forever.
            int u_idx = dst.id >= 0 ? dst.id : (int)(&dst - &out.ams[0]);
            if (u_idx < 0 || u_idx >= 4) u_idx = 0;
            String sig = tr.tray_type + "|" + tr.tray_info_idx + "|" + tr.tray_color;
            bool now_populated = tr.tray_type.length() ||
                                 tr.tray_info_idx.length() ||
                                 tr.tray_color.length();
            bool fire_pending = false;
            if (_slotSignatureSeen[u_idx][t]) {
                bool was_empty = _lastSlotSignature[u_idx][t].isEmpty() ||
                                 _lastSlotSignature[u_idx][t] == "||";
                if (sig != _lastSlotSignature[u_idx][t]) {
                    if (now_populated && was_empty) {
                        fire_pending = true;  // empty → populated: PendingAms claim
                    } else if (!now_populated && !was_empty) {
                        // populated → empty: physical unload. Drop the
                        // override so the slot doesn't still claim to hold
                        // the old spool (especially important for overrides
                        // auto-set by PendingAms on the last insertion).
                        Serial.printf("[Bambu %s] slot %d/%d physically unloaded\n",
                                      _cfg.serial.c_str(), u_idx, t);
                        const PrinterConfig* cfg_ro = g_printers_cfg.find(_cfg.serial);
                        if (cfg_ro && cfg_ro->findAmsOverride(u_idx, t).length()) {
                            PrinterConfig cfg_copy = *cfg_ro;
                            cfg_copy.setAmsOverride(u_idx, t, "");
                            g_printers_cfg.upsert(cfg_copy);
                            g_printers_cfg.save();
                            _cfg = cfg_copy;
                        }
                    }
                }
                // Swap (populated → different populated) is intentionally
                // NOT auto-claimed — too easy to mistake an unrelated
                // printer-panel edit for a real reload.
            }
            _lastSlotSignature[u_idx][t] = sig;
            _slotSignatureSeen[u_idx][t] = true;

            // Resolution order:
            //   1. Manual override on the PrinterConfig for this (ams, slot)
            //      — the user explicitly pinned a spool to this slot.
            //   2. Automatic tag_uid lookup against the local spool store —
            //      the Bambu RFID tag matches a spool we've scanned before.
            // If neither hits, mapped_spool_id stays empty and the UI shows
            // the slot as unmapped.
            tr.mapped_spool_id     = "";
            tr.mapped_via_override = false;
            String override_id = _cfg.findAmsOverride(u, t);
            if (override_id.length()) {
                // Only honour the override if the spool still exists.
                SpoolRecord rec;
                if (g_store.findById(override_id, rec)) {
                    tr.mapped_spool_id     = override_id;
                    tr.mapped_via_override = true;
                }
            }
            if (tr.mapped_spool_id.isEmpty() && tr.tag_uid.length()) {
                SpoolRecord rec;
                if (g_store.findByTagId(tr.tag_uid, rec)) tr.mapped_spool_id = rec.id;
            }

            // Sync the printer's tray_color/type/temps to match this spool's
            // definition whenever the mapping lands on a new spool_id (or
            // log an unload when it goes empty).
            int u_sync = dst.id >= 0 ? dst.id : (int)(&dst - &out.ams[0]);
            if (u_sync >= 0 && u_sync < 4) _syncSpoolToPrinter(u_sync, t, tr);

            // Run the PendingAms claim last so it can record the new mapping
            // into _lastSyncedSpoolId and avoid a double-push on the next tick.
            if (fire_pending) {
                _maybeAssignPendingSpool(u_idx, t);
            }

            ++t;
        }
        // Blank any remaining tray slots we didn't see in the payload.
        for (; t < 4; ++t) dst.trays[t] = AmsTray{};
        ++u;
    }
    out.ams_count = u;
    // Blank remaining AMS units.
    for (; u < 4; ++u) out.ams[u] = AmsUnit{};
}

void BambuPrinter::_maybeAssignPendingSpool(int ams_unit, int slot_id) {
    String spool_id;
    if (!PendingAms::claim(spool_id)) return;   // nothing armed, or already claimed by another printer

    SpoolRecord rec;
    if (!g_store.findById(spool_id, rec)) {
        Serial.printf("[Bambu %s] pending spool %s vanished -- skip push\n",
                      _cfg.serial.c_str(), spool_id.c_str());
        return;
    }

    Serial.printf("[Bambu %s] pending spool %s -> AMS %d slot %d\n",
                  _cfg.serial.c_str(), spool_id.c_str(), ams_unit, slot_id);
    _pushAmsFilamentSetting(ams_unit, slot_id, rec);
    // Record the sync so _syncSpoolToPrinter on the next tick (when the
    // override resolves mapped_spool_id) doesn't re-push the same payload.
    if (ams_unit == 254) {
        _lastSyncedSpoolIdVt = spool_id;
    } else if (ams_unit >= 0 && ams_unit < 4 && slot_id >= 0 && slot_id < 4) {
        _lastSyncedSpoolId[ams_unit][slot_id] = spool_id;
    }

    // Persist the mapping so print-consumption accounting routes future prints
    // to this spool even if the printer report clears tag_uid mid-print.
    // PrintersConfig exposes const-find + upsert — read, mutate, write back.
    const PrinterConfig* cfg_ro = g_printers_cfg.find(_cfg.serial);
    if (cfg_ro) {
        PrinterConfig cfg_copy = *cfg_ro;
        cfg_copy.setAmsOverride(ams_unit, slot_id, spool_id);
        g_printers_cfg.upsert(cfg_copy);
        g_printers_cfg.save();
        _cfg = cfg_copy;  // keep our resolver in sync
    }

    // Surface the result on the spool screen's AMS line so the user gets
    // confirmation. UI will clear the line on the next screen transition.
    char buf[64];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_OK "  AMS " LV_SYMBOL_RIGHT " %s slot %d",
             _cfg.name.length() ? _cfg.name.c_str() : _cfg.serial.c_str(), slot_id);
    ui_set_spool_ams_status(buf);

    // Notify main.cpp so the LCD's spool-detail screen can dismiss itself
    // shortly after the user sees the confirmation — no second interaction
    // needed once the spool is physically confirmed in an AMS tray.
    if (s_onSpoolAssigned) {
        s_onSpoolAssigned(spool_id,
                          _cfg.name.length() ? _cfg.name : _cfg.serial,
                          ams_unit, slot_id);
    }
}

void BambuPrinter::_syncSpoolToPrinter(int ams_unit, int slot_id, const AmsTray& tr) {
    // Edge-triggered: fire only when the resolved mapping changes. Two cases:
    //   * non-empty → non-empty (incl. empty → loaded): push ams_filament_setting
    //   * non-empty → empty (unload):                    clear the saved
    //     per-slot override so the slot doesn't keep masquerading as holding
    //     the old spool when a different one gets inserted next. We don't
    //     push anything to the printer on unload — blanking its tray fields
    //     would surprise users who manually set them.
    String& last = (ams_unit == 254)
                 ? _lastSyncedSpoolIdVt
                 : _lastSyncedSpoolId[ams_unit][slot_id];
    if (tr.mapped_spool_id == last) return;
    const String prev = last;
    last = tr.mapped_spool_id;

    if (tr.mapped_spool_id.isEmpty()) {
        // Unload: drop the auto-learned override. Leaves the spool record
        // itself untouched (last-known weight, K history etc. survive).
        Serial.printf("[Bambu %s] slot %d/%d unloaded (was spool %s)\n",
                      _cfg.serial.c_str(), ams_unit, slot_id, prev.c_str());
        const PrinterConfig* cfg_ro = g_printers_cfg.find(_cfg.serial);
        if (cfg_ro && cfg_ro->findAmsOverride(ams_unit, slot_id).length()) {
            PrinterConfig cfg_copy = *cfg_ro;
            cfg_copy.setAmsOverride(ams_unit, slot_id, "");
            g_printers_cfg.upsert(cfg_copy);
            g_printers_cfg.save();
            _cfg = cfg_copy;
            Serial.printf("[Bambu %s] cleared ams-override for slot %d/%d\n",
                          _cfg.serial.c_str(), ams_unit, slot_id);
        }
        return;
    }

    SpoolRecord rec;
    if (!g_store.findById(tr.mapped_spool_id, rec)) return;
    // If the printer already reports exactly the same colour (RGB, ignoring
    // alpha) AND material, skip the push. Saves a round-trip on every boot
    // when the printer already remembers what the last session set.
    bool color_matches = false;
    if (rec.color_code.length() >= 6 && tr.tray_color.length() >= 6) {
        color_matches = tr.tray_color.substring(0, 6)
                                     .equalsIgnoreCase(rec.color_code.substring(0, 6));
    }
    bool material_matches = rec.material_type.length() == 0 ||
                            rec.material_type.equalsIgnoreCase(tr.tray_type);
    if (color_matches && material_matches) {
        Serial.printf("[Bambu %s] slot %d/%d already in sync with spool %s\n",
                      _cfg.serial.c_str(), ams_unit, slot_id, rec.id.c_str());
        return;
    }

    Serial.printf("[Bambu %s] syncing slot %d/%d to spool %s (%s %s)\n",
                  _cfg.serial.c_str(), ams_unit, slot_id, rec.id.c_str(),
                  rec.material_type.c_str(), rec.color_code.c_str());
    _pushAmsFilamentSetting(ams_unit, slot_id, rec);
}

void BambuPrinter::_pushAmsFilamentSetting(int ams_unit, int slot_id, const SpoolRecord& rec) {
    if (!_mqtt || !_mqtt->connected()) {
        Serial.printf("[Bambu %s] ams push skipped — not connected\n", _cfg.serial.c_str());
        return;
    }

    uint32_t tmin = 0, tmax = 0;
    resolveTemps(rec.material_type, rec.nozzle_temp_min, rec.nozzle_temp_max, tmin, tmax);

    // Bambu expects RGBA. Pad RRGGBB → RRGGBBFF (fully opaque) if the
    // record only stored six hex chars, which is the SpoolHard
    // convention. Uppercase the whole string — Bambu's MQTT parser
    // accepts our mixed-case "ffde0aFF" silently but then drops it
    // back to "000000FF" in the next AMS report (we end up sending
    // material+tray_info_idx successfully but the colour reverts to
    // black). Uppercase RGBA matches what Bambu Studio sends and gets
    // honoured in the printer state.
    String color = rec.color_code;
    if (color.length() == 6) color += "FF";
    if (color.isEmpty())     color  = "FFFFFFFF";
    color.toUpperCase();

    JsonDocument doc;
    JsonObject print = doc["print"].to<JsonObject>();
    print["command"]         = "ams_filament_setting";
    print["ams_id"]          = ams_unit;
    print["tray_id"]         = slot_id;       // within-AMS slot (newer firmware variant)
    print["slot_id"]         = slot_id;
    print["tray_info_idx"]   = rec.slicer_filament;  // may be "" — printer keeps old idx
    print["tray_color"]      = color;
    print["nozzle_temp_min"] = tmin;
    print["nozzle_temp_max"] = tmax;
    print["tray_type"]       = rec.material_type;
    print["sequence_id"]     = "1";

    String payload;
    serializeJson(doc, payload);
    String topic = "device/" + _cfg.serial + "/request";
    _mqtt->publish(topic.c_str(), payload.c_str());
    Serial.printf("[Bambu %s] ams_filament_setting → AMS %d slot %d (%s %s, %u-%u°C)\n",
                  _cfg.serial.c_str(), ams_unit, slot_id,
                  rec.material_type.c_str(), color.c_str(), tmin, tmax);
}

// ----------------------------------------------------------------------------
// M3.4/3.5 — FTP + ZIP + gcode analyzer orchestration.
//
// Bambu stores the current print job in /cache/<name>.3mf on the printer's
// FTPS server. We stream just the plate gcode entry into the analyzer
// without landing the full 3MF on flash — the userfs partition is only
// 7 MB and Bambu jobs easily exceed that. The workflow:
//
//   1. FTP connect + SIZE.
//   2. Fetch the last ~64 KB to locate the EOCD → central directory offset.
//   3. Fetch the central directory, find the first *.gcode entry.
//   4. Fetch its local-header to resolve the actual data offset (variable
//      because of extra-field padding).
//   5. Range-read compressed_size bytes; for stored (method=0) entries
//      they go straight to the analyzer, for deflate (method=8) they
//      are piped through ROM-miniz tinfl_decompress with a 32 KiB
//      sliding dict in PSRAM and the inflated bytes feed the analyzer.
// ----------------------------------------------------------------------------

static IPAddress _parseIp(const String& s) {
    IPAddress ip;
    if (!ip.fromString(s)) return IPAddress(0,0,0,0);
    return ip;
}

// Pull the filename out of one BusyBox-style `ls -l` line. Bambu's FTPS
// emits e.g. `-rw-r--r-- 1 root root 12345 Jan 1 00:00 Some Job.3mf`
// — the filename is everything after column 8. Bare-name listings
// (no spaces) just pass through. Returns "" on lines we can't parse.
static String _ftpListName(const String& raw) {
    String line = raw; line.trim();
    if (!line.length()) return "";
    // Heuristic: if the line has no spaces it's already a bare name.
    if (line.indexOf(' ') < 0) return line;
    // Walk past 8 whitespace-separated tokens (perms, links, owner,
    // group, size, mon, day, time/year), then the rest is the name —
    // which may itself contain spaces, so we can't just `lastIndexOf`.
    int idx = 0, tokens = 0;
    while (idx < (int)line.length() && tokens < 8) {
        while (idx < (int)line.length() && line[idx] != ' ') idx++;
        while (idx < (int)line.length() && line[idx] == ' ') idx++;
        tokens++;
    }
    if (idx >= (int)line.length()) {
        // Fallback for non-`-l` formats (NLST returns bare names too).
        int sp = line.lastIndexOf(' ');
        return (sp >= 0) ? line.substring(sp + 1) : line;
    }
    String name = line.substring(idx);
    name.trim();
    return name;
}

// List `dir` and return the most plausible *.3mf match for `hint` — the
// MQTT subtask_name when we have one. Matching is fuzzy: we lowercase
// both sides and treat spaces/underscores/dashes as the same character,
// because Bambu sometimes sanitizes one but not the other when writing
// the file vs. populating subtask_name (ha-bambulab #1520, and the
// "filename has parens but the on-disk copy doesn't" case the user
// just hit). When nothing matches the hint, return the last *.3mf in
// the listing — Bambu's `LIST` sorts oldest first, so the active job
// tends to sit at the bottom. Returns "" if the listing has no .3mf
// entries or if the LIST command itself fails.
static String _normalizeForMatch(const String& s) {
    String out; out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') c = '_';
        out += c;
    }
    return out;
}
static String _bestThreeMfIn(PrinterFtp& ftp, const String& dir, const String& hint) {
    std::vector<String> lines;
    if (!ftp.listDir(dir, lines)) {
        dlog("ftp", "listDir(%s) failed: %s", dir.c_str(), ftp.lastError().c_str());
        return "";
    }
    dlog("ftp", "listDir(%s) returned %u entries", dir.c_str(), (unsigned)lines.size());
    String last3mf;
    String best;
    String hintLower = _normalizeForMatch(hint);
    if (hintLower.endsWith(".3mf")) hintLower = hintLower.substring(0, hintLower.length() - 4);
    for (const String& raw : lines) {
        String name = _ftpListName(raw);
        if (name.length() < 5) continue;
        String lower = name; lower.toLowerCase();
        if (!lower.endsWith(".3mf")) continue;
        last3mf = name;
        if (hintLower.length()) {
            String norm = _normalizeForMatch(name);
            if (norm.endsWith(".3mf")) norm = norm.substring(0, norm.length() - 4);
            if (norm == hintLower || norm.indexOf(hintLower) >= 0 ||
                hintLower.indexOf(norm) >= 0) {
                best = name;
                dlog("ftp", "matched %s ~ %s", hint.c_str(), name.c_str());
            }
        }
    }
    if (!best.length() && last3mf.length()) {
        dlog("ftp", "no fuzzy match for '%s' — using last *.3mf '%s'",
             hint.c_str(), last3mf.c_str());
        best = last3mf;
    }
    if (!best.length()) return "";
    return dir.endsWith("/") ? (dir + best) : (dir + "/" + best);
}

bool BambuPrinter::analyseRemote(const String& requestedPath) {
    if (_analysisInProgress) {
        _lastAnalysis.error = "analysis already in progress";
        return false;
    }
    _analysisInProgress = true;

    // Work directly on `_lastAnalysis` rather than a stack-local
    // GcodeAnalysis: the struct now contains a 101×16 float table
    // (~6.4 KiB) and a second local copy would burn the analyse-task's
    // stack twice over (paired with the ~6.4 KiB analyzer struct below).
    // The web /analysis endpoint reads _analysisInProgress first, so it
    // sees "still running" for the whole window we're mutating these
    // fields — no torn reads in practice.
    GcodeAnalysis& result = _lastAnalysis;
    result = GcodeAnalysis{};
    result.started_ms = millis();
    // Surface the resolved path below; if requestedPath was empty we
    // overwrite this once we know what we're actually fetching.
    result.path       = requestedPath;

    auto fail = [&](const String& why) -> bool {
        result.finished_ms  = millis();
        result.error        = why;
        result.valid        = false;
        _analysisInProgress = false;
        Serial.printf("[Bambu %s] analysis failed: %s\n",
                      _cfg.serial.c_str(), why.c_str());
        // Memory-pressure failures (TLS context couldn't be allocated)
        // are almost always transient — the print just started and MQTT
        // is still in its post-connect burst. Schedule a retry from the
        // main loop, with backoff, up to a generous cap.
        if (why.indexOf("Memory allocation") >= 0 ||
            why.indexOf("analyzer alloc")    >= 0) {
            if (_analysisAttempts < 8) {
                uint32_t backoff_ms = 8000UL * (1 + _analysisAttempts);
                _analysisNextRetryAt = millis() + backoff_ms;
                Serial.printf("[Bambu %s] scheduling analyse retry #%u in %lus\n",
                              _cfg.serial.c_str(),
                              (unsigned)(_analysisAttempts + 1),
                              (unsigned long)(backoff_ms / 1000));
            }
        }
        return false;
    };

    if (_cfg.ip.isEmpty() || _cfg.access_code.isEmpty()) return fail("no ip/access_code");

    IPAddress ip = _parseIp(_cfg.ip);
    PrinterFtp ftp;
    // Bambu's FTPS server cert has CN=<serial>; without matching SNI it
    // handshakes but silently drops post-banner commands. See printer_ftp.cpp.
    if (!ftp.connect(ip, _cfg.access_code, _cfg.serial)) {
        return fail("ftp connect: " + ftp.lastError());
    }

    // Resolve the actual 3MF path. Two modes:
    //   - Caller supplied a path (web FTP-debug flow): use it verbatim.
    //   - Auto-resolve: walk a candidate ladder built from MQTT
    //     subtask_name + gcode_file with .3mf and .gcode.3mf suffixes,
    //     under both /cache/ (cloud/MakerWorld convention) and / (LAN
    //     convention). SIZE-test each — control-channel only, so this
    //     works even when Bambu's PASV data port refuses connections
    //     (which it sometimes does on H2D — see ha-bambulab #1520 and
    //     the data-channel notes in printer_ftp.cpp). The mapping
    //     here mirrors ha-bambulab's `_attempt_ftp_download` ladder.
    String path = requestedPath;
    int32_t total = -1;
    if (path.length()) {
        total = ftp.size(path);
    } else {
        std::vector<String> candidates;
        auto addCandidates = [&candidates](const String& nameRaw) {
            if (!nameRaw.length()) return;
            String name = nameRaw;
            // gcode_file is a ramdisk path on X1 LAN prints — not FTP-
            // reachable, so skip anything containing /data/ or Metadata.
            if (name.indexOf("/data/") >= 0 || name.indexOf("Metadata") >= 0) return;
            // If it already has a path prefix, try it as-is too.
            if (name.startsWith("/")) {
                candidates.push_back(name);
                int slash = name.lastIndexOf('/');
                name = name.substring(slash + 1);
            }
            // Two suffix conventions and two search dirs (/cache then /).
            // ha-bambulab finds the LAN-print variant lives under /cache
            // with the ".gcode.3mf" suffix, while cloud/MakerWorld uses
            // ".3mf" only. Try the no-suffix variant when the field
            // already ends in ".3mf" so we don't accidentally double it.
            const char* dirs[] = {"/cache/", "/"};
            for (const char* dir : dirs) {
                if (name.endsWith(".3mf")) {
                    candidates.push_back(String(dir) + name);
                } else if (name.endsWith(".gcode")) {
                    // Already a raw-gcode filename — try as-is.
                    candidates.push_back(String(dir) + name);
                } else {
                    // Some H2D firmwares store the active job as a raw
                    // .gcode (no 3MF wrapper) — `<name>_plate_<N>.gcode`
                    // under /cache. Try those first since they're far
                    // more common in practice on this firmware than
                    // the .3mf variants. Also keep the .3mf candidates
                    // for cloud/MakerWorld jobs.
                    candidates.push_back(String(dir) + name + "_plate_1.gcode");
                    candidates.push_back(String(dir) + name + "_plate_2.gcode");
                    candidates.push_back(String(dir) + name + ".gcode");
                    candidates.push_back(String(dir) + name + ".3mf");
                    candidates.push_back(String(dir) + name + ".gcode.3mf");
                }
            }
        };
        addCandidates(_state.subtask_name);
        if (_state.gcode_file.length() && _state.gcode_file != _state.subtask_name) {
            addCandidates(_state.gcode_file);
        }
        // Two-pass scan: first accept only sz > 0 (the happy case where
        // SIZE reports the real length); then accept sz == 0 (Bambu
        // sometimes under-reports SIZE for in-progress or just-finished
        // prints — the file is there and RETR returns bytes anyway, so
        // we'd rather try than give up early).
        String zeroFallback;
        for (const String& c : candidates) {
            int32_t sz = ftp.size(c);
            dlog("ftp", "SIZE %s -> %ld", c.c_str(), (long)sz);
            if (sz > 0) { path = c; total = sz; break; }
            if (sz == 0 && !zeroFallback.length()) zeroFallback = c;
        }
        if (total <= 0 && zeroFallback.length()) {
            path = zeroFallback;
            total = 0;
            dlog("ftp", "all SIZE>0 attempts missed; trying %s with size=0",
                 path.c_str());
        }
        // Last-ditch: try listing /cache (works on the rare days the
        // PASV data port plays nice). We don't gate the candidate scan
        // behind this because LIST is exactly the operation that's been
        // failing — but if it IS up, picking the best match here beats
        // a confusing "could not find any of N candidates" error.
        if (total < 0) {
            String listed = _bestThreeMfIn(ftp, "/cache", _state.subtask_name);
            if (listed.length()) {
                int32_t sz = ftp.size(listed);
                dlog("ftp", "listing pick SIZE %s -> %ld", listed.c_str(), (long)sz);
                if (sz >= 0) { path = listed; total = sz; }
            }
        }
    }
    result.path = path;
    if (total < 0) {
        ftp.quit();
        if (!path.length()) {
            return fail("no current job: subtask_name+gcode_file both empty, listing failed");
        }
        return fail("SIZE failed for " + path);
    }
    // Pre-compute the byte range we'll feed into the analyzer below.
    // Two cases — Bambu prints can land on disk as either a ZIP-wrapped
    // 3MF (which we have to parse to find the gcode entry) or a raw
    // `.gcode` file (just stream + feed). Detection is purely by the
    // resolved path's extension.
    bool is_raw_gcode = path.endsWith(".gcode");

    uint32_t data_offset      = 0;
    uint32_t data_length      = 0;   // 0 means "stream until EOF" (raw)
    uint16_t data_method      = 0;   // 0=stored, 8=deflate (only set on .3mf path)

    if (!is_raw_gcode) {
        // ── ZIP-wrapped 3MF path ─────────────────────────────────
        // Grab the trailing EOCD window. ZIP's EOCD record is 22 fixed
        // bytes plus up to 65535 bytes of comment; Bambu's 3mf never
        // sets a ZIP comment, so 4 KiB is ample and keeps the backing
        // std::vector small enough that the internal heap stays
        // healthy for the simultaneous mbedTLS handshake context
        // (~30 KiB) on the same task.
        uint32_t eocd_window = (total > 0 && (uint32_t)total < 4096) ? (uint32_t)total : 4096;
        uint32_t eocd_start = 0;
        std::vector<uint8_t> eocdBuf(eocd_window);
        if (total > 0) {
            eocd_start = total - eocd_window;
            if (!ftp.retrieveInto(path, eocd_start, eocd_window, eocdBuf.data())) {
                ftp.quit();
                return fail("eocd fetch: " + ftp.lastError());
            }
        } else {
            // SIZE returned 0 but the file exists (Bambu's H2D firmware
            // does this for the active print's 3MF). RETR still streams
            // the real bytes — pull them all and keep the trailing
            // eocd_window for the EOCD parse below.
            uint32_t streamed = 0, tailGot = 0;
            if (!ftp.retrieveTrailing(path, eocd_window, eocdBuf.data(),
                                      &streamed, &tailGot)) {
                ftp.quit();
                return fail("trailing fetch: " + ftp.lastError());
            }
            if (streamed == 0) {
                ftp.quit();
                return fail("file empty for " + path +
                            " (SIZE 0 + RETR returned no bytes); "
                            "cache may have been rotated post-finish — "
                            "retry while gcode_state is RUNNING");
            }
            total       = (int32_t)streamed;
            eocd_window = tailGot;
            eocd_start  = streamed - tailGot;
            dlog("ftp", "trailing-stream got %u bytes; tail %u from offset %u",
                 (unsigned)streamed, (unsigned)tailGot, (unsigned)eocd_start);
        }

        uint32_t cd_offset = 0, cd_size = 0;
        uint16_t entry_count = 0;
        if (!ZipReader::parseEOCD(eocdBuf.data(), eocd_window, cd_offset, cd_size, entry_count)) {
            ftp.quit();
            return fail("EOCD not found");
        }

        std::vector<uint8_t> cdBuf(cd_size);
        if (!ftp.retrieveInto(path, cd_offset, cd_size, cdBuf.data())) {
            ftp.quit();
            return fail("cd fetch: " + ftp.lastError());
        }

        auto entries = ZipReader::parseCentralDirectory(cdBuf.data(), cd_size, entry_count);
        // Pick the first .gcode entry. Bambu's layout puts plate_<n>.gcode under
        // Metadata/ or directly at the root depending on slicer version.
        ZipReader::Entry gcode;
        bool found = false;
        for (auto& e : entries) {
            if (e.name.endsWith(".gcode")) { gcode = e; found = true; break; }
        }
        if (!found) { ftp.quit(); return fail("no .gcode entry"); }
        // Bambu's slicer ships some 3MFs with the gcode entry stored
        // (method=0) and others deflate-compressed (method=8). Both
        // are handled below; anything else (bzip2, etc.) we don't.
        if (gcode.method != 0 && gcode.method != 8) {
            ftp.quit();
            return fail("gcode entry has unsupported compression method=" +
                        String(gcode.method));
        }
        data_method = gcode.method;

        // Resolve local-header → data offset. Bambu often inserts an extra field
        // so compressed data doesn't start at local_header_offset + 30 + name_len.
        uint8_t lhdr[64];
        if (!ftp.retrieveInto(path, gcode.local_header_offset, sizeof(lhdr), lhdr)) {
            ftp.quit();
            return fail("local header fetch: " + ftp.lastError());
        }
        uint16_t name_len = 0, extra_len = 0;
        if (!ZipReader::parseLocalHeader(lhdr, sizeof(lhdr), name_len, extra_len)) {
            ftp.quit();
            return fail("local header parse");
        }
        data_offset = gcode.local_header_offset + 30 + name_len + extra_len;
        data_length = gcode.compressed_size;
    } else {
        // ── Raw .gcode path ─────────────────────────────────────
        // No wrapper — the entire file IS the gcode. data_offset stays
        // at 0; data_length stays at 0 to mean "stream to EOF" since
        // SIZE may be unreliable mid-print on some Bambu firmwares.
        dlog("ftp", "raw gcode path — skipping ZIP walk for %s", path.c_str());
    }

    // Seed per-tool densities so mm → grams conversion uses the right value
    // per slot. Priority:
    //   1. The mapped SpoolRecord's `density` — set by the filaments-library
    //      picker from Bambu's per-preset `filament_density` property, which
    //      distinguishes e.g. PLA Basic (1.24) from PLA-CF (1.28) and PLA
    //      Silk (1.27). Way more accurate than material-family defaults.
    //   2. Material-family fallback table below — covers the common cases
    //      when no library entry (or no spool at all) is mapped to the slot.
    // GCodeAnalyzer carries a 101×16 float `_mmAtPct` table (~6.4 KiB).
    // Allocate it in PSRAM to keep the analyse-task's internal-DRAM
    // footprint small — the mbedTLS handshake context alone wants ~30 KiB
    // of contiguous internal RAM and will fail with SSL - Memory allocation
    // failed if we've already burned the budget on big locals.
    auto* analyzer_mem = heap_caps_malloc(sizeof(GCodeAnalyzer), MALLOC_CAP_SPIRAM);
    if (!analyzer_mem) return fail("analyzer alloc failed");
    GCodeAnalyzer* analyzer_obj = new (analyzer_mem) GCodeAnalyzer();
    struct AnalyzerGuard {
        GCodeAnalyzer* p;
        ~AnalyzerGuard() { if (p) { p->~GCodeAnalyzer(); heap_caps_free(p); } }
    } analyzer_guard{analyzer_obj};
    GCodeAnalyzer& analyzer = *analyzer_obj;
    // GCodeAnalyzer has no user-declared constructor — its `_diameters[]`
    // and `_densities[]` C arrays are left uninitialised by the default
    // ctor on placement-new into PSRAM. reset() seeds them with the
    // 1.75 mm / 1.24 g/cm³ defaults before we override per-tool below.
    analyzer.reset();
    for (int u = 0; u < _state.ams_count; ++u) {
        for (int t = 0; t < 4; ++t) {
            const AmsTray& tr = _state.ams[u].trays[t];
            int idx = u * 4 + t;
            if (idx >= 16) break;

            float density = 0.f;
            if (tr.mapped_spool_id.length()) {
                SpoolRecord rec;
                if (g_store.findById(tr.mapped_spool_id, rec) && rec.density > 0.f) {
                    density = rec.density;
                }
            }
            if (density <= 0.f) {
                // Family fallback. Keep these in sync with Bambu's
                // resolved `filament_density` defaults.
                density = 1.24f;  // PLA
                if      (tr.tray_type == "ABS")   density = 1.04f;
                else if (tr.tray_type == "PETG")  density = 1.27f;
                else if (tr.tray_type == "PET-CF")density = 1.30f;
                else if (tr.tray_type == "PA")    density = 1.14f;
                else if (tr.tray_type == "PC")    density = 1.20f;
                else if (tr.tray_type == "TPU")   density = 1.21f;
                else if (tr.tray_type == "ASA")   density = 1.07f;
            }
            analyzer.setDensity(idx, density);
        }
    }

    // Stream the gcode bytes through the analyzer.
    //   * Raw .gcode files (data_length == 0): no ZIP wrap, full stream
    //     from byte 0 to EOF.
    //   * Stored .3mf entries (method=0): range-read the slice, feed
    //     verbatim to the analyzer.
    //   * Deflated .3mf entries (method=8): range-read the compressed
    //     slice, pipe each chunk through tinfl_decompress (32 KiB
    //     PSRAM sliding dict + ~11 KiB tinfl_decompressor state) and
    //     feed the decompressed bytes to the analyzer. tinfl handles
    //     output wrap-around for us by returning the per-call written
    //     length; we just have to split the feed across the ring's
    //     wrap point when it occurs.
    bool ok;
    if (data_length == 0) {
        ok = ftp.retrieveStream(path, [&](const uint8_t* d, size_t n, size_t) {
            analyzer.feed(d, n);
            return true;
        });
    } else if (data_method == 0) {
        ok = ftp.retrieveRange(path, data_offset, data_length,
                               [&](const uint8_t* d, size_t n) {
            analyzer.feed(d, n);
            return true;
        });
    } else {
        // Deflate path. Allocate state in PSRAM to keep internal DRAM
        // free for the concurrent FTPS data context.
        auto* inflator = (tinfl_decompressor*)heap_caps_malloc(
            sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        auto* dict = (uint8_t*)heap_caps_malloc(
            TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!inflator || !dict) {
            if (inflator) heap_caps_free(inflator);
            if (dict)     heap_caps_free(dict);
            ftp.quit();
            return fail("inflate buffer alloc failed");
        }
        tinfl_init(inflator);
        struct InflateState {
            tinfl_decompressor* inflator;
            uint8_t*            dict;
            size_t              dict_pos;     // next-write cursor in the ring (0..32K-1)
            uint32_t            consumed_in;  // bytes fed in so far (vs data_length)
            uint32_t            total_in;     // == data_length, snapshot for HAS_MORE_INPUT flag
            GCodeAnalyzer*      analyzer;
            bool                ok;
            String              err;
        } st = { inflator, dict, 0, 0, data_length, &analyzer, true, "" };

        ok = ftp.retrieveRange(path, data_offset, data_length,
                               [&st](const uint8_t* d, size_t n) {
            const uint8_t* in_ptr = d;
            size_t         in_remain = n;
            st.consumed_in += n;
            // Inner loop: a single FTP chunk may contain bytes that
            // produce multiple output windows (e.g. when the
            // decompressed stream is much larger than the compressed
            // chunk), so call tinfl_decompress until either the input
            // is fully consumed or the inflater says it needs more.
            while (in_remain > 0) {
                size_t in_size  = in_remain;
                size_t out_size = TINFL_LZ_DICT_SIZE - st.dict_pos;
                uint32_t flags = (st.consumed_in < st.total_in)
                                ? TINFL_FLAG_HAS_MORE_INPUT : 0;
                tinfl_status status = tinfl_decompress(
                    st.inflator,
                    in_ptr, &in_size,
                    st.dict, st.dict + st.dict_pos, &out_size,
                    flags);

                // Feed the freshly-written window to the analyzer.
                // The dict is treated as a ring; if a single tinfl
                // call's output crossed the end of the buffer the
                // bytes are split into two contiguous slices.
                size_t end = st.dict_pos + out_size;
                if (end <= TINFL_LZ_DICT_SIZE) {
                    if (out_size) st.analyzer->feed(st.dict + st.dict_pos, out_size);
                } else {
                    size_t first = TINFL_LZ_DICT_SIZE - st.dict_pos;
                    st.analyzer->feed(st.dict + st.dict_pos, first);
                    st.analyzer->feed(st.dict, out_size - first);
                }
                st.dict_pos = (st.dict_pos + out_size) & (TINFL_LZ_DICT_SIZE - 1);

                in_ptr    += in_size;
                in_remain -= in_size;

                if (status == TINFL_STATUS_DONE) return true;        // stop reading
                if (status == TINFL_STATUS_NEEDS_MORE_INPUT) break;  // wait for next chunk
                if (status == TINFL_STATUS_HAS_MORE_OUTPUT) continue; // re-call to flush
                if (status < 0) {
                    st.ok = false;
                    st.err = "tinfl status " + String((int)status);
                    return false;
                }
            }
            return true;
        });
        heap_caps_free(inflator);
        heap_caps_free(dict);
        if (!st.ok) {
            ftp.quit();
            return fail("inflate: " + st.err);
        }
    }
    // Before tearing the FTP session down, try to fetch the print's
    // `.bbl` companion. Bambu stores it next to each gcode in /cache
    // (~400-byte JSON manifest) and it carries the canonical
    // slicer-idx → AMS-tray-id mapping under the "ams mapping" key —
    // saves us guessing via filament_ids/colour for the common case.
    // Path transform:
    //   /cache/<name>_plate_<N>.gcode  →  /cache/<N>_<name>.gcode.bbl
    std::vector<int> bblAmsMapping;
    {
        String bblPath;
        if (path.endsWith(".gcode")) {
            int slash = path.lastIndexOf('/');
            String dir   = (slash >= 0) ? path.substring(0, slash + 1) : "";
            String fname = (slash >= 0) ? path.substring(slash + 1)    : path;
            int plateAt = fname.lastIndexOf("_plate_");
            if (plateAt > 0) {
                String name   = fname.substring(0, plateAt);
                String suffix = fname.substring(plateAt + 7);   // "<N>.gcode"
                int dot = suffix.indexOf('.');
                if (dot > 0) {
                    String plateNum = suffix.substring(0, dot);
                    bblPath = dir + plateNum + "_" + name + ".gcode.bbl";
                }
            }
        }
        if (bblPath.length()) {
            int32_t bblSize = ftp.size(bblPath);
            // .bbl is always tiny (<2 KB in practice). Cap at 8 KB to
            // protect against unexpected blobs.
            if (bblSize > 0 && bblSize <= 8192) {
                std::vector<uint8_t> bblBuf(bblSize + 1);
                if (ftp.retrieveInto(bblPath, 0, (uint32_t)bblSize, bblBuf.data())) {
                    bblBuf[bblSize] = 0;
                    JsonDocument bblDoc;
                    if (!deserializeJson(bblDoc, (const char*)bblBuf.data())) {
                        JsonArrayConst arr = bblDoc["ams mapping"].as<JsonArrayConst>();
                        if (arr) {
                            for (JsonVariantConst v : arr) bblAmsMapping.push_back(v.as<int>());
                            dlog("ftp", "%s parsed: %d entries", bblPath.c_str(),
                                 (int)bblAmsMapping.size());
                        }
                    }
                }
            } else {
                dlog("ftp", ".bbl skipped: SIZE %ld for %s",
                     (long)bblSize, bblPath.c_str());
            }
        }
    }
    ftp.quit();
    if (!ok) return fail("gcode fetch: " + ftp.lastError());

    analyzer.finalise();

    // Helper: resolve a slicer tool-idx → physical AMS tray index.
    //   * Returns 0..15 for an AMS slot, 254 for vt_tray, -1 for unknown.
    //   * Priority: .bbl `ams mapping` (canonical) → gcode header
    //     filament_ids match → header filament_colour match →
    //     _state.active_tray (single-tool fallback).
    auto resolveTray = [&](int tool_idx) -> int {
        // 1. .bbl mapping
        if (tool_idx < (int)bblAmsMapping.size()) {
            int v = bblAmsMapping[tool_idx];
            if (v >= 0) return v;   // -1 means "unused"; fall through to fallbacks
        }
        // 2. filament_ids → tray_info_idx
        const auto& slot = analyzer.slicerSlot(tool_idx);
        if (slot.filament_id.length()) {
            for (int u2 = 0; u2 < _state.ams_count; ++u2) {
                for (int s = 0; s < 4; ++s) {
                    if (_state.ams[u2].trays[s].tray_info_idx == slot.filament_id) {
                        return u2 * 4 + s;
                    }
                }
            }
            if (_state.has_vt_tray && _state.vt_tray.tray_info_idx == slot.filament_id) {
                return 254;
            }
        }
        // 3. filament_colour → tray_color (RGBA prefix-match, case-insensitive)
        if (slot.color_rgb != 0) {
            auto trayMatchesColour = [&](const AmsTray& tr) {
                if (tr.tray_color.length() < 6) return false;
                uint32_t c = (uint32_t)strtoul(
                    tr.tray_color.substring(0, 6).c_str(), nullptr, 16);
                return c == slot.color_rgb;
            };
            for (int u2 = 0; u2 < _state.ams_count; ++u2) {
                for (int s = 0; s < 4; ++s) {
                    if (trayMatchesColour(_state.ams[u2].trays[s])) return u2 * 4 + s;
                }
            }
            if (_state.has_vt_tray && trayMatchesColour(_state.vt_tray)) return 254;
        }
        // 4. Single-tool fallback: whatever the printer says is feeding now.
        if (_state.active_tray >= 0 && _state.active_tray < 16) {
            return _state.active_tray;
        }
        if (_state.active_tray == 254 && _state.has_vt_tray) return 254;
        return -1;
    };

    // Pack results + correlate tools with AMS slots and spool ids for the UI.
    int emitted = 0;
    for (int i = 0; i < GCodeAnalyzer::MAX_TOOLS && emitted < 16; ++i) {
        const auto& u = analyzer.tool(i);
        if (u.mm <= 0.f) continue;
        GcodeAnalysisTool out;
        out.tool_idx = i;
        out.mm       = u.mm;
        out.grams    = u.grams;
        int tray = resolveTray(i);
        if (tray == 254 && _state.has_vt_tray) {
            out.ams_unit = 254;
            out.slot_id  = 0;
            out.spool_id = _state.vt_tray.mapped_spool_id;
            out.material = _state.vt_tray.tray_type;
            out.color    = _state.vt_tray.tray_color;
        } else if (tray >= 0 && tray < 16) {
            int u2 = tray / 4, s = tray % 4;
            if (u2 < _state.ams_count) {
                const AmsTray& tr = _state.ams[u2].trays[s];
                out.ams_unit = u2;
                out.slot_id  = s;
                out.spool_id = tr.mapped_spool_id;
                out.material = tr.tray_type;
                out.color    = tr.tray_color;
            }
        }
        result.tools[emitted++] = out;
    }
    result.tool_count   = emitted;
    result.total_mm     = analyzer.totalMm();
    result.total_grams  = analyzer.totalGrams();

    // Copy the per-percent grams lookup so live-consume tracking can report
    // exact filament usage at each progress boundary rather than linearly
    // extrapolating from the total. Indexed by the physical tool number so
    // it matches GcodeAnalysisTool::tool_idx.
    if (analyzer.hasPercentTable()) {
        result.has_pct_table = true;
        for (int p = 0; p <= 100; ++p) {
            for (int t = 0; t < 16; ++t) {
                result.grams_at_pct[p][t] = analyzer.gramsAtPct(p, t);
            }
        }
    }
    result.finished_ms  = millis();
    result.valid        = true;
    // `result` is already a reference to `_lastAnalysis`, so no copy needed.
    _analysisInProgress = false;

    Serial.printf("[Bambu %s] analysis ok: %d tools, total %.1fg%s\n",
                  _cfg.serial.c_str(), emitted, result.total_grams,
                  result.has_pct_table ? " [M73 table]" : " [no M73 — linear]");

    // Forward the per-tool breakdown to the scale so it can update its own
    // consumption bookkeeping for trays mapped to local spools. Gated on the
    // per-printer track_print_consume flag.
    if (_cfg.track_print_consume) {
        JsonDocument tools;
        JsonArray arr = tools.to<JsonArray>();
        for (int i = 0; i < result.tool_count; ++i) {
            const auto& t = result.tools[i];
            JsonObject row = arr.add<JsonObject>();
            row["tool_idx"] = t.tool_idx;
            row["grams"]    = t.grams;
            row["spool_id"] = t.spool_id;
            row["material"] = t.material;
        }
        g_scale.pushGcodeAnalysis(_cfg.serial, result.total_grams, tools);
    }
    return true;
}

// ----------------------------------------------------------------------------
// Live print-consume tracking.
//
// Flow:
//   1. gcode_state goes IDLE/PREPARE → RUNNING:
//      - Clear _analysisCommitted so a new print's totals can be committed.
//      - If track_print_consume is on, spawn a background FreeRTOS task that
//        runs analyseRemote(). We can't block MQTT's parsing loop with the
//        FTP+ZIP fetch, so it goes on core 0 just like the manual trigger.
//   2. gcode_state goes RUNNING/PAUSE → FINISH (or IDLE/FAILED):
//      - If a valid analysis exists and we haven't already, commit per-tool
//        grams into each mapped spool's consumed_since_add and
//        consumed_since_weight and persist. weight_current is NOT touched —
//        that field is reserved for actual weighings from the scale.
// ----------------------------------------------------------------------------

static bool _isActiveGcodeState(const String& s) {
    return s == "RUNNING" || s == "PAUSE";
}

static bool _isTerminalGcodeState(const String& s) {
    // "IDLE" on its own isn't terminal (it's the at-rest state), but a
    // transition FROM an active state INTO IDLE means the print ended
    // without hitting FINISH (e.g. user-stopped). Treat those as terminal.
    return s == "FINISH" || s == "FAILED" || s == "IDLE";
}

struct AnalyseTaskCtx {
    BambuPrinter* printer;
    String        path;
};

static void _analyseTaskTrampoline(void* arg) {
    auto* ctx = static_cast<AnalyseTaskCtx*>(arg);
    ctx->printer->analyseRemote(ctx->path);
    delete ctx;
    s_analyseSlot.busy = false;   // safe no-op when running on internal stack
    vTaskDelete(nullptr);
}

bool BambuPrinter::startAnalyseTask(const String& path) {
    auto* ctx = new AnalyseTaskCtx{this, path};
    bool ok = spawnPSRAMFallbackTask(_analyseTaskTrampoline, ctx, "ana",
                                     /*core*/0, /*priority*/1, s_analyseSlot);
    if (!ok) {
        delete ctx;
        Serial.printf("[Bambu %s] startAnalyseTask: both internal + PSRAM "
                      "stack alloc failed\n", _cfg.serial.c_str());
    }
    return ok;
}

void BambuPrinter::_maybeRetryAnalysis() {
    if (_analysisNextRetryAt == 0) return;
    if ((int32_t)(millis() - _analysisNextRetryAt) < 0) return;
    if (_analysisInProgress) return;
    if (_lastAnalysis.valid) { _analysisNextRetryAt = 0; return; }
    if (!_cfg.track_print_consume) { _analysisNextRetryAt = 0; return; }
    if (!_isActiveGcodeState(_state.gcode_state)) { _analysisNextRetryAt = 0; return; }

    _analysisNextRetryAt = 0;
    ++_analysisAttempts;
    Serial.printf("[Bambu %s] retrying analyseRemote (attempt %u)\n",
                  _cfg.serial.c_str(), (unsigned)_analysisAttempts);
    // Empty path → analyseRemote auto-resolves from MQTT subtask_name
    // with an FTP-listing fallback. The old "/cache/.3mf" hardcode was
    // a leftover guess that 550'd as soon as Bambu started naming the
    // job something other than empty.
    if (!startAnalyseTask("")) {
        Serial.printf("[Bambu %s] analyse retry task alloc failed\n",
                      _cfg.serial.c_str());
        // Re-arm a longer retry so we don't spin on task-alloc failures.
        _analysisNextRetryAt = millis() + 30000;
    }
}

// ── Interactive FTP debug ─────────────────────────────────────────
// Pairs with the Debug page on the console web UI. Every protocol step
// (connect, TLS handshake, banner, USER, PASS, PASV, LIST / RETR, each
// data chunk) is serialised as an NDJSON `{"kind":"trace",...}` line
// into a shared FreeRTOS stream buffer; a final `{"kind":"done",...}`
// line carries the overall result. The web handler pipes the stream
// buffer into a chunked HTTP response, so the browser reads events
// progressively via `fetch().body.getReader()` with natural TCP-window
// backpressure — no per-client message-queue limits like the old
// AsyncWebSocket broadcast suffered from (208 LIST entries used to
// silently drop to 1).
//
// Three operations:
//   probe    — connect + auth, no data transfer
//   list     — connect + auth + LIST <path>
//   download — connect + auth + RETR <path> → writes to SD:/ftp_dl.bin;
//              chunk-progress emitted every ~500 ms. UI can then link
//              to GET /api/ftp-download to save the file.
#include <SD.h>
#include "config.h"       // SD_MOUNT

BambuPrinter::FtpStreamCtx::FtpStreamCtx() {
    // 64 KiB so the biggest line we'll ever emit (the `list` done event
    // with the full /cache directory inline — ~25 KiB on a saturated
    // printer) fits in one xStreamBufferSend call. The previous 8 KiB
    // size was short-writing the done line and the frontend's NDJSON
    // parser silently skipped the malformed JSON, leaving the spinner
    // spinning forever. FreeRTOS streams use the system heap (internal
    // DRAM); 64 KiB sits comfortably in the ~100 KiB free pool we hold
    // during a transfer and avoids the retry-loop complexity entirely.
    sb = xStreamBufferCreate(65536, 1);
}
BambuPrinter::FtpStreamCtx::~FtpStreamCtx() {
    if (sb) vStreamBufferDelete(sb);
}
void BambuPrinter::FtpStreamCtx::emit(const JsonDocument& doc) {
    if (!sb) return;
    String out;
    serializeJson(doc, out);
    out += '\n';
    // 5 s timeout — guards against a disappeared client (consumer
    // stops draining) leaving the FTP task blocked. Single-shot is
    // safe with the 64 KiB buffer above: every line we emit fits
    // whole-or-not, never half-written, so the NDJSON stream can't
    // be left in a parseable-prefix-with-malformed-tail state.
    xStreamBufferSend(sb, out.c_str(), out.length(), pdMS_TO_TICKS(5000));
}

struct FtpDebugCtx {
    BambuPrinter* printer;
    String op;
    String path;
    std::shared_ptr<BambuPrinter::FtpStreamCtx> sink;
};

namespace {
void emitTrace(BambuPrinter::FtpStreamCtx& s,
               const String& serial, const String& op, const char* step,
               int code, const String& text, uint32_t elapsed_ms) {
    JsonDocument doc;
    doc["kind"]       = "trace";
    doc["serial"]     = serial;
    doc["op"]         = op;
    doc["step"]       = step;
    doc["code"]       = code;
    doc["text"]       = text;
    doc["elapsed_ms"] = elapsed_ms;
    s.emit(doc);
}
void emitDone(BambuPrinter::FtpStreamCtx& s,
              const String& serial, const String& op, bool ok,
              const String& message) {
    JsonDocument doc;
    doc["kind"]    = "done";
    doc["serial"]  = serial;
    doc["op"]      = op;
    doc["ok"]      = ok;
    doc["message"] = message;
    s.emit(doc);
}
// "Done with extra payload" — caller has already populated `payload`
// (e.g. with a list-entries array or a download-file descriptor) and we
// just merge it into the envelope.
void emitDoneWith(BambuPrinter::FtpStreamCtx& s,
                  const String& serial, const String& op, bool ok,
                  const String& message, const JsonDocument& payload) {
    JsonDocument doc;
    doc["kind"]    = "done";
    doc["serial"]  = serial;
    doc["op"]      = op;
    doc["ok"]      = ok;
    doc["message"] = message;
    doc["payload"] = payload;
    s.emit(doc);
}
}  // namespace

static void _ftpDebugTrampoline(void* arg) {
    auto* ctx = static_cast<FtpDebugCtx*>(arg);
    ctx->printer->_runFtpDebug(ctx->op, ctx->path, ctx->sink);
    delete ctx;
    s_ftpDebugSlot.busy = false;
    vTaskDelete(nullptr);
}

bool BambuPrinter::startFtpDebug(const String& op, const String& path,
                                 std::shared_ptr<FtpStreamCtx> sink) {
    if (_ftpDebugBusy) return false;
    if (!sink || !sink->sb) return false;
    _ftpDebugBusy = true;
    auto* ctx = new FtpDebugCtx{this, op, path, sink};
    bool ok = spawnPSRAMFallbackTask(_ftpDebugTrampoline, ctx, "ftpdbg",
                                     /*core*/0, /*priority*/1, s_ftpDebugSlot);
    if (!ok) {
        delete ctx;
        _ftpDebugBusy = false;
        Serial.printf("[Bambu %s] ftp-debug task alloc failed (internal+PSRAM)\n",
                      _cfg.serial.c_str());
        return false;
    }
    return true;
}

void BambuPrinter::_runFtpDebug(const String& op, const String& path,
                                std::shared_ptr<FtpStreamCtx> sink) {
    const String& serial = _cfg.serial;
    auto& s = *sink;
    emitTrace(s, serial, op, "start", 0, "op=" + op + " path=" + path, 0);

    if (_cfg.ip.isEmpty() || _cfg.access_code.isEmpty()) {
        emitDone(s, serial, op, false, "no ip/access_code configured");
        s.done = true;
        _ftpDebugBusy = false;
        return;
    }
    IPAddress ip = _parseIp(_cfg.ip);
    PrinterFtp ftp;
    // Capture `sink` shared_ptr (not just a raw reference to s) so the
    // trace lambda keeps the ctx alive for its lifetime — PrinterFtp
    // owns this callback until its dtor runs at scope exit.
    ftp.setTraceCb([sink, serial, op](const char* step, int code,
                                      const String& text, uint32_t ms) {
        emitTrace(*sink, serial, op, step, code, text, ms);
    });
    if (!ftp.connect(ip, _cfg.access_code, serial)) {
        emitDone(s, serial, op, false, String("connect failed: ") + ftp.lastError());
        s.done = true;
        _ftpDebugBusy = false;
        return;
    }

    if (op == "probe") {
        emitDone(s, serial, op, true, "login complete");
    } else if (op == "list") {
        std::vector<String> entries;
        String listPath = path.length() ? path : String("/cache");
        bool ok = ftp.listDir(listPath, entries);
        if (!ok) {
            emitDone(s, serial, op, false, String("list failed: ") + ftp.lastError());
        } else {
            JsonDocument payload;
            JsonArray arr = payload["entries"].to<JsonArray>();
            for (auto& e : entries) arr.add(e);
            emitDoneWith(s, serial, op, true,
                         String(entries.size()) + " entries",
                         payload);
        }
    } else if (op == "download") {
        if (path.isEmpty()) {
            emitDone(s, serial, op, false, "download: path required");
            ftp.quit();
            s.done = true;
            _ftpDebugBusy = false;
            return;
        }
        // SD.open paths are bare `/foo`, NOT `/sd/foo` — the SD library's
        // own VFS mount strips the prefix and a wrong path silently fails
        // on writes (and on exists()/remove(), giving the false "doesn't
        // exist" answer that lets us fall through to open() and 0).
        const char* dest_path = "/ftp_dl.bin";
        if (SD.exists(dest_path)) SD.remove(dest_path);
        File out = SD.open(dest_path, FILE_WRITE);
        if (!out) {
            emitDone(s, serial, op, false, String("open ") + dest_path + " failed");
            ftp.quit();
            s.done = true;
            _ftpDebugBusy = false;
            return;
        }
        size_t total_bytes = 0;
        uint32_t last_report_ms = 0;
        bool ok = ftp.retrieveStream(path, [&](const uint8_t* d, size_t n, size_t t) {
            size_t w = out.write(d, n);
            total_bytes = t;
            if (w != n) return false;
            if (millis() - last_report_ms > 500) {
                last_report_ms = millis();
                emitTrace(s, serial, op, "chunk", (int)total_bytes, "", 0);
            }
            return true;
        });
        out.close();
        if (!ok) {
            emitDone(s, serial, op, false, String("download failed: ") + ftp.lastError());
        } else {
            JsonDocument payload;
            payload["path"]  = path;
            payload["bytes"] = (uint32_t)total_bytes;
            payload["url"]   = "/api/ftp-download";
            emitDoneWith(s, serial, op, true,
                         String(total_bytes) + " bytes written to ftp_dl.bin",
                         payload);
        }
    } else {
        emitDone(s, serial, op, false, "unknown op");
    }

    ftp.quit();
    s.done = true;             // signals the HTTP filler to end the response
    _ftpDebugBusy = false;
}

void BambuPrinter::_handleGcodeStateTransition(const String& prev, const String& now) {
    Serial.printf("[Bambu %s] gcode_state %s -> %s\n",
                  _cfg.serial.c_str(),
                  prev.isEmpty() ? "<none>" : prev.c_str(),
                  now.c_str());

    // Print started.
    if (!_isActiveGcodeState(prev) && _isActiveGcodeState(now)) {
        _analysisCommitted = false;
        _progressCommittedPct = 0;
        _analysisNextRetryAt  = 0;
        _analysisAttempts     = 0;
        if (_cfg.track_print_consume && !_analysisInProgress) {
            Serial.printf("[Bambu %s] print started — queuing background analysis\n",
                          _cfg.serial.c_str());
            // Empty path → analyseRemote auto-resolves from MQTT
            // subtask_name with an FTP-listing fallback. The old
            // "/cache/.3mf" hardcode was a leftover guess that 550'd
            // as soon as Bambu started naming the job something other
            // than empty. startAnalyseTask handles the spawn (internal
            // stack first, PSRAM-static stack on fallback) so the
            // RUNNING-edge auto-trigger doesn't get starved by the
            // contiguous-internal-DRAM crunch we hit mid-print.
            if (!startAnalyseTask("")) {
                Serial.printf("[Bambu %s] analysis task alloc failed\n",
                              _cfg.serial.c_str());
            }
        }
        return;
    }

    // Print ended (terminal transition). Commit consumption once.
    if (_isActiveGcodeState(prev) && _isTerminalGcodeState(now)) {
        if (_analysisCommitted) return;
        _commitPrintConsumption();
        _analysisCommitted = true;
    }
}

// Compute the grams of tool `t` extruded between pcts `from` and `to`. Uses
// the exact M73 table when the analysis captured one; otherwise falls back
// to a linear fraction of `t.grams`.
static float _deltaGrams(const GcodeAnalysis& a, const GcodeAnalysisTool& t, int from, int to) {
    if (to <= from) return 0.f;
    if (a.has_pct_table && t.tool_idx >= 0 && t.tool_idx < 16) {
        float g0 = a.grams_at_pct[from][t.tool_idx];
        float g1 = a.grams_at_pct[to][t.tool_idx];
        float d  = g1 - g0;
        return d > 0.f ? d : 0.f;
    }
    return (float)(to - from) / 100.f * t.grams;
}

void BambuPrinter::_commitPrintConsumption() {
    if (!_lastAnalysis.valid) {
        Serial.printf("[Bambu %s] print ended but no valid analysis — nothing to commit\n",
                      _cfg.serial.c_str());
        return;
    }
    // Live commits in _commitIncrementalConsumption may have already pushed
    // part of the total forecast. Only add the unclaimed tail so end-of-print
    // totals match the analysis regardless of how many 5% steps fired.
    int from = _progressCommittedPct;
    int to   = 100;
    if (to <= from) {
        Serial.printf("[Bambu %s] print ended — all consumption already committed live\n",
                      _cfg.serial.c_str());
        return;
    }
    int committed = 0;
    for (int i = 0; i < _lastAnalysis.tool_count; ++i) {
        const auto& t = _lastAnalysis.tools[i];
        if (t.grams <= 0.f) continue;
        if (t.spool_id.isEmpty()) continue;   // no spool mapped to this tool

        SpoolRecord rec;
        if (!g_store.findById(t.spool_id, rec)) continue;
        float add = _deltaGrams(_lastAnalysis, t, from, to);
        if (add <= 0.f) continue;
        // consumed_since_* are forecasts, not measurements, so we ADD grams
        // rather than replace. weight_current stays untouched — it only
        // changes on an actual scale reading via the "capture current weight"
        // action.
        rec.consumed_since_add    += add;
        rec.consumed_since_weight += add;
        g_store.upsert(rec);
        ++committed;
        Serial.printf("[Bambu %s] +%.1fg consumed on spool %s (T%d, %s) [tail %d%%→100%%]\n",
                      _cfg.serial.c_str(), add, rec.id.c_str(),
                      t.tool_idx, t.material.c_str(), from);
    }
    _progressCommittedPct = 100;
    Serial.printf("[Bambu %s] commitPrintConsumption: %d spool(s) updated\n",
                  _cfg.serial.c_str(), committed);
}

void BambuPrinter::_commitIncrementalConsumption() {
    if (!_cfg.track_print_consume) return;
    if (!_isActiveGcodeState(_state.gcode_state)) return;
    if (!_lastAnalysis.valid) return;                 // analysis still running
    int pct = _state.progress_pct;
    if (pct < 0 || pct > 100) return;
    // Quantise to 5% steps so we flush the spool store at most 20× per
    // print — LittleFS is wear-sensitive and each upsert rewrites the full
    // record as an append. The final tail is closed by _commitPrintConsumption
    // on the terminal state transition.
    int stepTarget = (pct / 5) * 5;
    if (stepTarget <= _progressCommittedPct) return;
    int from = _progressCommittedPct;
    int to   = stepTarget;
    int committed = 0;
    for (int i = 0; i < _lastAnalysis.tool_count; ++i) {
        const auto& t = _lastAnalysis.tools[i];
        if (t.grams <= 0.f) continue;
        if (t.spool_id.isEmpty()) continue;
        SpoolRecord rec;
        if (!g_store.findById(t.spool_id, rec)) continue;
        float add = _deltaGrams(_lastAnalysis, t, from, to);
        if (add <= 0.f) continue;
        rec.consumed_since_add    += add;
        rec.consumed_since_weight += add;
        g_store.upsert(rec);
        ++committed;
    }
    _progressCommittedPct = stepTarget;
    if (committed > 0) {
        Serial.printf("[Bambu %s] live consume: %d%%→%d%% (%d spool(s), %s)\n",
                      _cfg.serial.c_str(), from, to, committed,
                      _lastAnalysis.has_pct_table ? "M73" : "linear");
    }
}
