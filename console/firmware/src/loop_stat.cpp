#include "loop_stat.h"
#include "spoolhard/ring_log.h"
#include <atomic>

namespace LoopStat {
namespace {

constexpr uint32_t kWindowMs = 10000;
const char* const kNames[S_COUNT] = {
    "wifi", "scale", "nfc", "sd", "crash_log", "bambu", "ssdp",
    "bambu_disc", "scale_disc", "ota_check",
    "ams_panel", "home_foot", "ota_banner", "devcfg", "ws_push", "tail"};

struct Win {
    uint32_t start_ms;
    uint32_t iters;
    uint64_t body_sum_us, delay_sum_us;
    uint32_t loop_max_us;           // body + delay
    uint32_t body_max_us;
    uint32_t step_max_us[S_COUNT];
    uint64_t step_sum_us[S_COUNT];
};

Win s_cur;
Win s_last;                          // finished window (valid when s_lastSeq>0)
uint32_t s_wall_ms = 0;
std::atomic<uint32_t> s_seq{0};      // seqlock: odd = writer active
uint32_t s_windows = 0;

}  // namespace

void add(Step s, uint32_t dt_us) {
    if (dt_us > s_cur.step_max_us[s]) s_cur.step_max_us[s] = dt_us;
    s_cur.step_sum_us[s] += dt_us;
}

void endIteration(uint32_t body_us, uint32_t delay_us) {
    uint32_t now = millis();
    if (s_cur.start_ms == 0) s_cur.start_ms = now;
    s_cur.iters++;
    s_cur.body_sum_us  += body_us;
    s_cur.delay_sum_us += delay_us;
    if (body_us > s_cur.body_max_us) s_cur.body_max_us = body_us;
    uint32_t whole = body_us + delay_us;
    if (whole > s_cur.loop_max_us) s_cur.loop_max_us = whole;

    uint32_t wall = now - s_cur.start_ms;
    if (wall < kWindowMs) return;

    // Publish (seqlock) then log.
    s_seq.fetch_add(1, std::memory_order_acq_rel);
    s_last = s_cur;
    s_wall_ms = wall;
    s_windows++;
    s_seq.fetch_add(1, std::memory_order_acq_rel);

    const Win& w = s_last;
    uint32_t iters = w.iters ? w.iters : 1;
    // Busy% in 0.1 % units: body time / wall time.
    uint32_t busy_pm = (uint32_t)((w.body_sum_us / 1000ULL) * 1000ULL / (wall ? wall : 1));
    uint32_t ips10 = (uint32_t)((uint64_t)w.iters * 10000ULL / (wall ? wall : 1));
    dlog("LoopStat",
         "win=%lums it/s=%lu.%lu busy=%lu.%lu%% loop_max=%lu.%lums avg_body=%luus avg_whole=%luus",
         (unsigned long)wall, (unsigned long)(ips10 / 10), (unsigned long)(ips10 % 10),
         (unsigned long)(busy_pm / 10), (unsigned long)(busy_pm % 10),
         (unsigned long)(w.loop_max_us / 1000), (unsigned long)((w.loop_max_us % 1000) / 100),
         (unsigned long)(w.body_sum_us / iters),
         (unsigned long)((w.body_sum_us + w.delay_sum_us) / iters));
    // Ring-log lines are capped at 159 chars, so the per-step numbers are
    // split across short lines (all tagged LoopStat).
#define U(x) (unsigned long)(x)
#define MX(i) U(w.step_max_us[i])
#define AV(i) U(w.step_sum_us[i] / iters)
    dlog("LoopStat", "max_us wifi=%lu scale=%lu nfc=%lu sd=%lu crash=%lu bambu=%lu ssdp=%lu",
         MX(S_WIFI), MX(S_SCALE), MX(S_NFC), MX(S_SD), MX(S_CRASH_LOG), MX(S_BAMBU), MX(S_SSDP));
    dlog("LoopStat", "max_us bdisc=%lu sdisc=%lu ota=%lu ams=%lu foot=%lu banner=%lu devcfg=%lu ws=%lu tail=%lu",
         MX(S_BAMBU_DISC), MX(S_SCALE_DISC), MX(S_OTA_CHECK), MX(S_AMS_PANEL), MX(S_HOME_FOOT),
         MX(S_OTA_BANNER), MX(S_DEVCFG), MX(S_WS_PUSH), MX(S_TAIL));
    dlog("LoopStat", "avg_us wifi=%lu scale=%lu nfc=%lu sd=%lu crash=%lu bambu=%lu ssdp=%lu bdisc=%lu sdisc=%lu ota=%lu tail=%lu",
         AV(S_WIFI), AV(S_SCALE), AV(S_NFC), AV(S_SD), AV(S_CRASH_LOG), AV(S_BAMBU), AV(S_SSDP),
         AV(S_BAMBU_DISC), AV(S_SCALE_DISC), AV(S_OTA_CHECK), AV(S_TAIL));
#undef U
#undef MX
#undef AV

    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.start_ms = millis();
}

void toJson(JsonDocument& doc) {
    Win w; uint32_t wall, nwin, a, b;
    do {
        a = s_seq.load(std::memory_order_acquire);
        w = s_last; wall = s_wall_ms; nwin = s_windows;
        b = s_seq.load(std::memory_order_acquire);
    } while ((a & 1) || a != b);
    JsonObject o = doc["loop_stat"].to<JsonObject>();
    o["windows"] = nwin;
    if (!nwin) return;
    uint32_t iters = w.iters ? w.iters : 1;
    o["window_ms"]     = wall;
    o["iters"]         = w.iters;
    o["iters_per_s_x10"] = (uint32_t)((uint64_t)w.iters * 10000ULL / (wall ? wall : 1));
    o["busy_pct_x10"]  = (uint32_t)((w.body_sum_us / 1000ULL) * 1000ULL / (wall ? wall : 1));
    o["loop_max_us"]   = w.loop_max_us;
    o["body_max_us"]   = w.body_max_us;
    o["avg_body_us"]   = (uint32_t)(w.body_sum_us / iters);
    o["avg_whole_us"]  = (uint32_t)((w.body_sum_us + w.delay_sum_us) / iters);
    JsonObject mx = o["step_max_us"].to<JsonObject>();
    JsonObject av = o["step_avg_us"].to<JsonObject>();
    for (int i = 0; i < S_COUNT; ++i) {
        mx[kNames[i]] = w.step_max_us[i];
        av[kNames[i]] = (uint32_t)(w.step_sum_us[i] / iters);
    }
}

}  // namespace LoopStat
