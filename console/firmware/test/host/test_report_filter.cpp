// Host test for the Bambu report deserialization filter (bambu_report_filter.h).
//
// For every report (real H2S captures + synthetic X1/P1/delta/ack shapes) it
// deserializes twice -- full and filtered -- and asserts that a digest of
// EVERYTHING the firmware reads from the report doc is byte-identical. It also
// scans bambu_printer.cpp for `["key"]` literals in the report-reading
// functions and fails if one is missing from the filter (drift guard).
//
// Run from the repo root (ArduinoJson path = any 7.x checkout/libdeps dir):
//   g++ -std=c++17 -O2 -I console/firmware/include -I <ArduinoJson>/src \
//     console/firmware/test/host/test_report_filter.cpp -o /tmp/trf && \
//     /tmp/trf console/firmware/test/host/fixtures console/firmware/src/bambu_printer.cpp [captures_dir]
// e.g. <ArduinoJson>/src = console/firmware/.pio/libdeps/esp32-s3/ArduinoJson/src
// The optional captures_dir holds report_*.json raw captures (not in the repo).
#include <ArduinoJson.h>
#include "bambu_report_filter.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- digest: mirrors every read in _recordCommandAck/_parseReport/_parseAms/
// ---- _parseExternalTray (and the ams_raw debug payload). ----
static void dumpTray(std::ostringstream& o, JsonVariantConst tray) {
    o << "id=" << (atoi(tray["id"] | "-1")) << ' '
      << (tray["tray_type"] | "") << '|' << (tray["tray_sub_brands"] | "") << '|'
      << (tray["tray_color"] | "") << '|' << (tray["tray_info_idx"] | "") << '|'
      << (tray["tag_uid"] | "") << '|';
    if (tray["nozzle_temp_min"].is<const char*>()) o << "nmin=" << atoi(tray["nozzle_temp_min"].as<const char*>()) << ' ';
    if (tray["nozzle_temp_max"].is<const char*>()) o << "nmax=" << atoi(tray["nozzle_temp_max"].as<const char*>()) << ' ';
    if (tray["remain"].is<int>())   o << "remain=" << tray["remain"].as<int>() << ' ';
    if (tray["k"].is<float>())      o << "k=" << tray["k"].as<float>() << ' ';
    if (tray["cali_idx"].is<int>()) o << "cali=" << tray["cali_idx"].as<int>() << ' ';
    o << ';';
}

static std::string digest(const JsonDocument& doc) {
    std::ostringstream o;
    JsonObjectConst print = doc["print"];
    o << "print=" << (bool)print << '\n';
    if (!print) return o.str();
    o << "fun=" << (print["fun"].is<const char*>() ? print["fun"].as<const char*>() : "-") << '\n';
    if (print["result"].is<const char*>() && print["command"].is<const char*>()) {
        o << "ack " << print["command"].as<const char*>() << ' ' << print["result"].as<const char*>()
          << " reason=" << (print["reason"] | "") << " err=" << print["err_code"].as<long>()
          << " seq=" << (print["sequence_id"] | "?") << '\n';
    }
    auto str = [&](const char* k) { if (print[k].is<const char*>()) o << k << '=' << print[k].as<const char*>() << '\n'; };
    str("gcode_state"); str("subtask_name"); str("gcode_file");
    if (print["task_id"].is<const char*>()) o << "task=" << print["task_id"].as<const char*>() << '\n';
    else if (print["task_id"].is<int>())    o << "task#" << print["task_id"].as<int>() << '\n';
    if (print["mc_percent"].is<int>())      o << "pct=" << print["mc_percent"].as<int>() << '\n';
    if (print["layer_num"].is<int>())       o << "layer=" << print["layer_num"].as<int>() << '\n';
    if (print["total_layer_num"].is<int>()) o << "tlayer=" << print["total_layer_num"].as<int>() << '\n';
    if (print["bed_temper"].is<float>())    o << "bed=" << print["bed_temper"].as<float>() << '\n';
    if (print["nozzle_temper"].is<float>()) o << "noz=" << print["nozzle_temper"].as<float>() << '\n';

    JsonObjectConst device = print["device"];
    o << "device=" << (bool)device << '\n';
    if (device) {
        JsonObjectConst nozzleInfo = device["nozzle"];
        o << "nozzle=" << (bool)nozzleInfo << '\n';
        if (nozzleInfo) {
            JsonArrayConst info = nozzleInfo["info"];
            int n = 0;
            for (JsonVariantConst nn : info) { if (n >= 2) break; o << "dia" << n << '=' << (nn["diameter"] | 0.f) << '\n'; ++n; }
            o << "nozzle_count=" << n << '\n';
        }
        JsonObjectConst extruder = device["extruder"];
        JsonArrayConst extInfo = extruder["info"];
        if (extInfo && extInfo.size() > 0) {
            int cur = ((extruder["state"] | 0) >> 4) & 0xF;
            JsonVariantConst e = extInfo[0];
            for (JsonVariantConst cand : extInfo) if ((cand["id"] | -1) == cur) { e = cand; break; }
            o << "ext cur=" << cur << " size=" << extInfo.size();
            if (e["snow"].is<int>()) o << " snow=" << e["snow"].as<int>();
            o << '\n';
        }
    }

    JsonObjectConst ams = print["ams"];
    o << "ams=" << (bool)ams << '\n';
    if (ams) {
        if (ams["tray_now"].is<const char*>()) o << "tray_now=" << atoi(ams["tray_now"].as<const char*>()) << '\n';
        JsonArrayConst units = ams["ams"];
        o << "units=" << (bool)units << '\n';
        int u = 0;
        for (JsonVariantConst unit : units) {
            if (u >= 4) break;
            o << "U" << u << " id=" << atoi(unit["id"] | "-1") << " hum=" << atoi(unit["humidity"] | "-1")
              << " raw=" << atoi(unit["humidity_raw"] | "-1");
            if (unit["temp"].is<const char*>()) o << " temp=" << atof(unit["temp"].as<const char*>());
            JsonObjectConst dry = unit["dry_setting"];
            if (dry) {
                if (dry["dry_duration"].is<int>())    o << " dd=" << dry["dry_duration"].as<int>();
                if (dry["dry_temperature"].is<int>()) o << " dt=" << dry["dry_temperature"].as<int>();
                o << " df=" << (dry["dry_filament"] | "");
            }
            o << '\n';
            JsonArrayConst trays = unit["tray"];
            int t = 0;
            for (JsonVariantConst tray : trays) { if (t >= 4) break; dumpTray(o, tray); ++t; }
            o << "\n";
            ++u;
        }
        // ams_raw debug payload
        std::string raw; serializeJson(ams, raw);
        o << "RAW_AMS " << raw << '\n';
        if (print["vt_tray"].is<JsonObjectConst>()) { std::string s; serializeJson(print["vt_tray"], s); o << "RAW_VT " << s << '\n'; }
        if (print["vir_slot"].is<JsonArrayConst>()) { std::string s; serializeJson(print["vir_slot"], s); o << "RAW_VIR " << s << '\n'; }
    }
    JsonObjectConst vt = print["vt_tray"];
    if (vt) { o << "VT "; dumpTray(o, vt); o << '\n'; }
    else {
        JsonArrayConst vir = print["vir_slot"];
        if (vir && vir.size() > 0) {
            o << "VIR n=" << vir.size() << ' ';
            for (JsonObjectConst e : vir) {
                o << "[id=" << atoi(e["id"] | "-1") << " type=" << (e["tray_type"] | "") << ' ';
                dumpTray(o, e); o << ']';
            }
            JsonObjectConst first = vir[0].as<JsonObjectConst>();
            o << " first_id=" << atoi(first["id"] | "255") << '\n';
        }
    }
    return o.str();
}

// Counts live + peak bytes of the document's node tree / strings (the doc's own
// allocator is the only thing that allocates; memoryUsage() is a no-op in 7.4).
struct CountingAlloc : ArduinoJson::Allocator {
    std::map<void*, size_t> live; size_t cur = 0, peak = 0;
    void* allocate(size_t n) override { void* p = malloc(n); live[p] = n; cur += n; if (cur > peak) peak = cur; return p; }
    void deallocate(void* p) override { if (!p) return; cur -= live[p]; live.erase(p); free(p); }
    void* reallocate(void* p, size_t n) override {
        size_t old = p ? live[p] : 0; void* q = realloc(p, n);
        if (p) live.erase(p);
        live[q] = n; cur += n - old; if (cur > peak) peak = cur; return q;
    }
};

struct Mem { size_t full = 0, filt = 0; double tfull = 0, tfilt = 0; };

static void runOne(const std::string& name, const std::string& json, Mem* m, bool verbose = false) {
    JsonDocument filter; bambuBuildReportFilter(filter);
    CountingAlloc af, at;
    JsonDocument full(&af), filt(&at);
    auto t0 = std::chrono::steady_clock::now();
    DeserializationError e1;
    for (int i = 0; i < 20; ++i) { full.clear(); e1 = deserializeJson(full, json); }
    auto t1 = std::chrono::steady_clock::now();
    DeserializationError e2;
    for (int i = 0; i < 20; ++i) { filt.clear(); e2 = deserializeJson(filt, json, DeserializationOption::Filter(filter)); }
    auto t2 = std::chrono::steady_clock::now();
    CHECK(!e1 && !e2, "%s: parse error full=%s filt=%s", name.c_str(), e1.c_str(), e2.c_str());
    std::string a = digest(full), b = digest(filt);
    CHECK(a == b, "%s: digest differs\n--- full ---\n%s\n--- filtered ---\n%s", name.c_str(), a.c_str(), b.c_str());
    if (m) {
        m->full += af.peak; m->filt += at.peak;
        m->tfull += std::chrono::duration<double, std::micro>(t1 - t0).count() / 20;
        m->tfilt += std::chrono::duration<double, std::micro>(t2 - t1).count() / 20;
    }
    if (verbose) printf("  %-28s len=%5zu live bytes full=%5zu filt=%5zu (peak %zu / %zu)\n", name.c_str(), json.size(), af.cur, at.cur, af.peak, at.peak);
}

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

static void flatKeys(JsonVariantConst v, std::set<std::string>& out) {
    if (v.is<JsonObjectConst>()) for (JsonPairConst kv : v.as<JsonObjectConst>()) { out.insert(kv.key().c_str()); flatKeys(kv.value(), out); }
    else if (v.is<JsonArrayConst>()) for (JsonVariantConst e : v.as<JsonArrayConst>()) flatKeys(e, out);
}

// Every `["lit"]` between the report-reading function starts and the next
// unrelated function must be a key of the filter (ams/vt_tray/vir_slot are
// whole-subtree, so keys beneath them are covered by `true`).
static void driftScan(const std::string& src) {
    JsonDocument filter; bambuBuildReportFilter(filter);
    std::set<std::string> keys; flatKeys(filter, keys);
    keys.insert("print");
    // keys that live below a `true` (whole-subtree) filter entry
    const std::set<std::string> under_true = {
        "tray_now", "ams", "id", "humidity", "humidity_raw", "temp", "dry_setting", "dry_duration",
        "dry_temperature", "dry_filament", "tray", "tray_type", "tray_sub_brands", "tray_color",
        "tray_info_idx", "tag_uid", "nozzle_temp_min", "nozzle_temp_max", "remain", "k", "cali_idx"};
    struct Range { const char* from; const char* to; };
    const Range ranges[] = {
        {"void BambuPrinter::_recordCommandAck", "void BambuPrinter::_parseReport"},
        {"void BambuPrinter::_parseReport", "void BambuPrinter::_autoRestoreKValues"},
        {"void BambuPrinter::_parseAms", "bool BambuPrinter::requestClaimPending"},
    };
    int scanned = 0;
    for (auto& r : ranges) {
        size_t a = src.find(r.from), b = src.find(r.to, a == std::string::npos ? 0 : a + 1);
        CHECK(a != std::string::npos && b != std::string::npos && b > a, "drift scan: range %s not found", r.from);
        if (a == std::string::npos || b == std::string::npos) continue;
        for (size_t i = a; i < b;) {
            size_t p = src.find("[\"", i);
            if (p == std::string::npos || p >= b) break;
            size_t q = src.find("\"]", p + 2);
            std::string k = src.substr(p + 2, q - p - 2);
            ++scanned;
            // keys read from the REPORT only; skip the debug payload writes (payload["x"]) and
            // reads of other docs by checking the preceding identifier.
            size_t s = p; while (s > a && (isalnum((unsigned char)src[s - 1]) || src[s - 1] == '_')) --s;
            std::string owner = src.substr(s, p - s);
            if (owner != "payload" && owner != "doc") {
                CHECK(keys.count(k) || under_true.count(k), "drift: report key \"%s\" (read via '%s') missing from filter", k.c_str(), owner.c_str());
            }
            i = q + 2;
        }
    }
    printf("drift scan: %d key literals checked against %zu filter keys\n", scanned, keys.size());
}

int main(int argc, char** argv) {
    std::string fixtures = argc > 1 ? argv[1] : "console/firmware/test/host/fixtures";
    std::string srcPath = argc > 2 ? argv[2] : "console/firmware/src/bambu_printer.cpp";
    std::string capDir = argc > 3 ? argv[3] : "";

    driftScan(slurp(srcPath));

    // ---- synthetic shapes ----
    const std::vector<std::pair<std::string, std::string>> syn = {
        {"x1_ams_running", R"({"print":{"command":"push_status","msg":1,"sequence_id":"100","gcode_state":"RUNNING","subtask_name":"benchy","gcode_file":"benchy.gcode","task_id":1948,"mc_percent":42,"layer_num":10,"total_layer_num":200,"bed_temper":60.0,"nozzle_temper":215.5,"hms":[{"attr":1,"code":2}],"lights_report":[{"node":"chamber_light","mode":"on"}],"fun":"35FF1AFFF9CB3","device":{"nozzle":{"info":[{"id":0,"diameter":0.4,"type":"HS00"}]}},"ams":{"tray_now":"2","tray_tar":"255","tray_exist_bits":"f","version":7,"ams":[{"id":"0","humidity":"4","humidity_raw":"35","temp":"29.9","info":"2003","dry_setting":{"dry_duration":0,"dry_temperature":0,"dry_filament":""},"tray":[{"id":"0","tray_type":"PLA","tray_sub_brands":"PLA Basic","tray_color":"FF0000FF","tray_info_idx":"GFA00","tag_uid":"0123456789ABCDEF","nozzle_temp_min":"190","nozzle_temp_max":"230","remain":88,"k":0.02,"cali_idx":3,"bed_temp":"55","xcam_info":"x","tray_uuid":"u"},{"id":"1"},{"id":"2","tray_type":"PETG","tray_color":"00FF00FF"},{"id":"3","tray_type":""}]},{"id":"1","humidity":"3","tray":[{"id":"0","tray_type":"ABS"}]}]},"vt_tray":{"id":"254","tray_type":"PLA","tray_color":"FFFFFFFF","tray_info_idx":"GFA00","tag_uid":"0000000000000000","remain":-1,"k":0.03,"cali_idx":-1,"nozzle_temp_min":"190","nozzle_temp_max":"230","tray_sub_brands":"x"}}})"},
        {"p1_vt_tray_now_255", R"({"print":{"command":"push_status","gcode_state":"IDLE","ams":{"tray_now":"255","ams":[]},"vt_tray":{"id":"254","tray_type":"","tray_color":""},"mc_percent":0,"bed_temper":24,"nozzle_temper":25}})"},
        {"finish", R"({"print":{"gcode_state":"FINISH","mc_percent":100,"subtask_name":"done","task_id":"77","upgrade_state":{"a":1},"ipcam":{"b":2},"xcam":{"c":3}}})"},
        {"failed", R"({"print":{"gcode_state":"FAILED","print_error":50348044,"mc_print_error_code":"1","stg_cur":0}})"},
        {"delta_tiny", R"({"print":{"nozzle_temper":210.2}})"},
        {"delta_only_other_keys", R"({"print":{"wifi_signal":"-40dBm","hms":[],"lights_report":[{"a":1}]}})"},
        {"delta_ams_unknown_only", R"({"print":{"ams":{"tray_exist_bits":"1","ams_exist_bits":"1"}}})"},
        {"delta_device_unknown_only", R"({"print":{"device":{"airduct":{"x":1},"nozzle":{"exist":1}}}})"},
        {"delta_empty_print", R"({"print":{}})"},
        {"info_cmd", R"({"info":{"command":"get_version","module":[{"name":"ota","sw_ver":"01.00"}],"result":"success"}})"},
        {"system_cmd", R"({"system":{"command":"get_access_code","sequence_id":"1","access_code":"12345678","result":"success"}})"},
        {"ack_ok", R"({"print":{"command":"ams_filament_setting","result":"success","reason":"","sequence_id":"2001","err_code":0,"ams_id":0}})"},
        {"ack_fail", R"({"print":{"command":"extrusion_cali_set","result":"fail","reason":"verify failed","err_code":84033543,"sequence_id":"5"}})"},
        {"ack_pushall", R"({"print":{"command":"pushall","result":"success","sequence_id":"1"}})"},
        {"h2d_two_extruders", R"({"print":{"gcode_state":"RUNNING","device":{"nozzle":{"exist":3,"info":[{"id":0,"diameter":0.4,"type":"HS01"},{"id":1,"diameter":0.6,"type":"HS01"},{"id":2,"diameter":0.8}]},"extruder":{"state":4113,"info":[{"id":0,"snow":65535,"hnow":1,"star":2},{"id":1,"snow":258,"hnow":3}]},"airduct":{"modeCur":1}},"vir_slot":[{"id":"254","tray_type":"PLA","tray_color":"FF0000FF","remain":50,"k":0.04,"cali_idx":2,"tray_info_idx":"GFA00","tag_uid":"AABB","nozzle_temp_min":"190","nozzle_temp_max":"240","tray_sub_brands":"b","cols":["161616FF"]},{"id":"255","tray_type":"","tray_color":""}],"ams":{"tray_now":"0","ams":[{"id":"128","humidity":"2","tray":[{"id":"0","tray_type":"PA"}]}]}}})"},
        {"h2s_extruder_ext", R"({"print":{"device":{"extruder":{"state":0,"info":[{"id":0,"snow":65280}]}},"vir_slot":[{"id":"255","tray_type":"PLA"}]}})"},
        {"extruder_empty_info", R"({"print":{"device":{"extruder":{"info":[]}}}})"},
        {"task_id_float_str", R"({"print":{"task_id":"1948","mc_percent":"5","layer_num":3.0,"nozzle_temper":"x","bed_temper":70}})"},
        {"ams_five_units", R"({"print":{"ams":{"ams":[{"id":"0","tray":[{"id":"0"},{"id":"1"},{"id":"2"},{"id":"3"},{"id":"4","tray_type":"PLA"}]},{"id":"1"},{"id":"2"},{"id":"3"},{"id":"4"}]}}})"},
    };
    Mem sm;
    for (auto& s : syn) runOne("syn:" + s.first, s.second, &sm);
    printf("synthetic: %zu reports\n", syn.size());

    // ---- committed fixture(s) ----
    int nfix = 0;
    if (DIR* d = opendir(fixtures.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".json") { runOne("fixture:" + n, slurp(fixtures + "/" + n), nullptr, true); ++nfix; }
        }
        closedir(d);
    }
    printf("fixtures: %d\n", nfix);

    // ---- optional raw captures ----
    if (!capDir.empty()) {
        Mem m; int n = 0;
        for (int i = 1; i <= 999; ++i) {
            char p[512]; snprintf(p, sizeof p, "%s/report_%03d.json", capDir.c_str(), i);
            std::string j = slurp(p); if (j.empty()) continue;
            runOne(p, j, &m, i == 1); ++n;
        }
        if (n) printf("captures: %d reports, avg peak doc bytes full=%zu filtered=%zu (%.1fx), avg parse time full=%.0fus filtered=%.0fus (ratio %.2f)\n",
                      n, m.full / n, m.filt / n, (double)m.full / m.filt, m.tfull / n, m.tfilt / n, m.tfilt / m.tfull);
    }
    if (g_fail) { printf("%d FAILURE(S)\n", g_fail); return 1; }
    puts("ok");
    return 0;
}
