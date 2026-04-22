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
#include <WiFiClientSecure.h>
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

    // Hand the blocking PubSubClient::connect() to a one-shot task. Pinned
    // to core 0 with low priority so it can't preempt the main loop on
    // core 1; main loop polls _connectTaskState each tick to harvest.
    _connectTaskResult = false;
    _connectTaskState  = ConnectTaskState::Pending;
    BaseType_t rc = xTaskCreatePinnedToCore(_connectTrampoline, "bambu_conn",
                                            16384, this, 1, &_connectTask, 0);
    if (rc != pdPASS) {
        // Couldn't spawn (heap pressure, mostly) — drop back to Idle so the
        // next 5-s retry tries again. Don't fall back to a synchronous
        // connect here, that's exactly the stall we're trying to escape.
        _connectTaskState = ConnectTaskState::Idle;
        _state.link       = BambuLinkState::Failed;
        _state.error_message = "task spawn failed";
        Serial.printf("[Bambu %s] connect task spawn failed\n", _cfg.serial.c_str());
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

    // Bambu expects RGBA. Pad RRGGBB → RRGGBBFF (fully opaque) if the record
    // only stored six hex chars, which is the SpoolHard convention.
    String color = rec.color_code;
    if (color.length() == 6) color += "FF";
    if (color.isEmpty())     color  = "FFFFFFFF";

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
// FTPS server. We stream just the plate gcode entry (stored, not deflated) into
// the analyzer without landing the full 3MF on flash — the userfs partition is
// only 7 MB and Bambu jobs easily exceed that. The workflow:
//
//   1. FTP connect + SIZE.
//   2. Fetch the last ~64 KB to locate the EOCD → central directory offset.
//   3. Fetch the central directory, find the first *.gcode entry.
//   4. Fetch its local-header to resolve the actual data offset (variable
//      because of extra-field padding).
//   5. Range-read the compressed_size bytes through the gcode analyzer.
//
// Deflate entries are currently rejected — Bambu stores the gcode entry so
// this covers the common case; adding a streaming inflator is a follow-up.
// ----------------------------------------------------------------------------

static IPAddress _parseIp(const String& s) {
    IPAddress ip;
    if (!ip.fromString(s)) return IPAddress(0,0,0,0);
    return ip;
}

bool BambuPrinter::analyseRemote(const String& path) {
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
    result.path       = path;

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

    int32_t total = ftp.size(path);
    if (total <= 0) { ftp.quit(); return fail("SIZE failed"); }

    // Grab the trailing EOCD window. ZIP's EOCD record is 22 fixed bytes
    // plus up to 65535 bytes of comment; Bambu's 3mf never sets a ZIP
    // comment, so 4 KiB is ample and keeps the backing std::vector small
    // enough that the internal heap stays healthy for the simultaneous
    // mbedTLS handshake context (~30 KiB) on the same task.
    const uint32_t eocd_window = total > 4096 ? 4096 : (uint32_t)total;
    uint32_t eocd_start = total - eocd_window;
    std::vector<uint8_t> eocdBuf(eocd_window);
    if (!ftp.retrieveInto(path, eocd_start, eocd_window, eocdBuf.data())) {
        ftp.quit();
        return fail("eocd fetch: " + ftp.lastError());
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
    if (gcode.method != 0) {
        ftp.quit();
        return fail("gcode entry is deflated (method=" + String(gcode.method) + "), not yet supported");
    }

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
    uint32_t data_offset = gcode.local_header_offset + 30 + name_len + extra_len;

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

    // Stream the compressed bytes (method=0 means raw) through the analyzer.
    bool ok = ftp.retrieveRange(path, data_offset, gcode.compressed_size,
                                [&](const uint8_t* d, size_t n) {
        analyzer.feed(d, n);
        return true;
    });
    ftp.quit();
    if (!ok) return fail("gcode range: " + ftp.lastError());

    analyzer.finalise();

    // Pack results + correlate tools with AMS slots and spool ids for the UI.
    int emitted = 0;
    for (int i = 0; i < GCodeAnalyzer::MAX_TOOLS && emitted < 16; ++i) {
        const auto& u = analyzer.tool(i);
        if (u.mm <= 0.f) continue;
        GcodeAnalysisTool out;
        out.tool_idx = i;
        out.mm       = u.mm;
        out.grams    = u.grams;
        int ams_u = i / 4, slot = i % 4;
        if (ams_u < _state.ams_count) {
            const AmsTray& tr = _state.ams[ams_u].trays[slot];
            out.ams_unit = ams_u;
            out.slot_id  = slot;
            out.spool_id = tr.mapped_spool_id;
            out.material = tr.tray_type;
            out.color    = tr.tray_color;
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
    vTaskDelete(nullptr);
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
    auto* ctx = new AnalyseTaskCtx{this, "/cache/.3mf"};
    BaseType_t rc = xTaskCreatePinnedToCore(_analyseTaskTrampoline, "ana",
                                            16384, ctx, 1, nullptr, 0);
    if (rc != pdPASS) {
        delete ctx;
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
    // 8 KiB is comfortably bigger than any single line we emit; bursty
    // events (e.g. 208-entry list payload) are still one ftp_done line.
    sb = xStreamBufferCreate(8192, 1);
}
BambuPrinter::FtpStreamCtx::~FtpStreamCtx() {
    if (sb) vStreamBufferDelete(sb);
}
void BambuPrinter::FtpStreamCtx::emit(const JsonDocument& doc) {
    if (!sb) return;
    String out;
    serializeJson(doc, out);
    out += '\n';
    // 5 s timeout guards against a disappeared client leaving us blocked
    // forever if the stream buffer fills — after that we silently give
    // up on this line and the task can exit cleanly.
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
    vTaskDelete(nullptr);
}

bool BambuPrinter::startFtpDebug(const String& op, const String& path,
                                 std::shared_ptr<FtpStreamCtx> sink) {
    if (_ftpDebugBusy) return false;
    if (!sink || !sink->sb) return false;
    _ftpDebugBusy = true;
    auto* ctx = new FtpDebugCtx{this, op, path, sink};
    BaseType_t rc = xTaskCreatePinnedToCore(_ftpDebugTrampoline, "ftpdbg",
                                            16384, ctx, 1, nullptr, 0);
    if (rc != pdPASS) {
        delete ctx;
        _ftpDebugBusy = false;
        Serial.printf("[Bambu %s] ftp-debug task alloc failed\n", _cfg.serial.c_str());
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
        const char* dest_path = SD_MOUNT "/ftp_dl.bin";
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
            auto* ctx = new AnalyseTaskCtx{this, "/cache/.3mf"};
            // 16 KiB stack — analyseRemote heap-allocates the analyzer into
            // PSRAM and writes the result directly into _lastAnalysis, so
            // the stack holds the PrinterFtp (with its WiFiClientSecure) plus
            // the ZIP parser's locals and mbedTLS handshake state. 8 KiB
            // overflowed the first time mbedtls tried a handshake; 16 KiB
            // leaves headroom without burning an internal-DRAM contiguous
            // block the size of 24 KiB (which the 30 KiB TLS ctx would then
            // fail to allocate on top of).
            BaseType_t rc = xTaskCreatePinnedToCore(_analyseTaskTrampoline, "ana",
                                                    16384, ctx, 1, nullptr, 0);
            if (rc != pdPASS) {
                delete ctx;
                Serial.printf("[Bambu %s] analysis task alloc failed\n", _cfg.serial.c_str());
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
