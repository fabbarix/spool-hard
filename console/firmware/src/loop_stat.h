#pragma once
// Whole-loop timing for the Arduino loopTask. Static counters only: no
// heap, no Strings, ~600 B of .bss. Every step is timed in microseconds;
// once per 10 s window one `[LoopStat]` line goes to the ring log (dlog)
// and the finished window is published for /api/heap ("loop_stat").
#include <Arduino.h>
#include <ArduinoJson.h>

namespace LoopStat {

enum Step : uint8_t {
    S_WIFI, S_SCALE, S_NFC, S_SD, S_CRASH_LOG, S_BAMBU, S_SSDP,
    S_BAMBU_DISC, S_SCALE_DISC, S_OTA_CHECK,
    S_AMS_PANEL, S_HOME_FOOT, S_OTA_BANNER, S_DEVCFG, S_WS_PUSH,
    S_TAIL,      // rest of loop() after the 1 Hz block (excl. delay)
    S_COUNT
};

// Record one step's duration (us). Called from loopTask only.
void add(Step s, uint32_t dt_us);
// Call once at the end of every loop() iteration. body_us = loop start to
// just before delay(); delay_us = time spent in delay().
void endIteration(uint32_t body_us, uint32_t delay_us);
// Append the latest finished window to a JSON doc (any task).
void toJson(JsonDocument& doc);

}  // namespace LoopStat
