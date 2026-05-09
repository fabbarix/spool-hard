#pragma once
#include "load_cell.h"
#include <functional>

// Single-owner FreeRTOS task wrapper around `g_scale` (LoadCell).
//
// Why: HX711 sampling, tare, multi-point calibration, and config
// reload are now interleaved with HTTP handlers, console-protocol
// dispatch, and the main loop's other work. With LoadCell owned by
// the loopTask, slow operations (`_readRawAveraged(20)` blocks up
// to ~10 s on a dead chip; saveCalibration writes NVS for ~100 ms;
// addCalPoint sorts the array) starve AsyncTCP and the WS heartbeat.
//
// Now: a dedicated `sensor_task` is the SOLE owner of the LoadCell
// instance. It runs the sample loop continuously and processes
// commands posted to its inbox. Other tasks read state via the
// publish-only Snapshot below or via convenience getters that
// project a single field (single 32-bit reads are atomic on ESP32
// — the seq counter guards multi-field reads from torn updates).
//
// API mirror of the old direct LoadCell calls so the migration is
// localised to `main.cpp` + `web_server.cpp` route handlers; the
// LoadCell class itself is unchanged.
namespace SensorTask {

// Atomic-publish snapshot of the live state. Each successful sample
// in the task increments `seq` by 2 (odd = mid-update, even = stable),
// allowing readers to detect torn reads via a Linux-style seqlock.
struct Snapshot {
    WeightState state    = WeightState::Uncalibrated;
    float       weight_g = 0.0f;
    long        last_raw = 0;
    int         precision = 1;
};

// Spawn the task. Idempotent — calling again is a no-op. Must be
// called after `g_scale.begin()` has loaded calibration + params.
// Stack 4 KB; priority 4 on core 1.
void begin();

// Read-only snapshot. Performs an internal seqlock retry if a sample
// happened to land mid-read. Always returns a self-consistent value.
Snapshot snapshot();

// Convenience accessors — same semantics as the old g_scale.getXxx().
WeightState getState();
float       getWeightG();
long        getLastRaw();
int         getPrecision();

// Calibration view. Brief mutex inside (calibration is rarely-mutated).
CalibrationData snapshotCal();

// Mutating commands. All return immediately — work is queued.
// Completion is signalled by the next CalibrationStatus push from
// main.cpp's coordinator loop.
void requestTare();
void requestCalibrate(float known_weight_g);   // legacy single-point
void requestAddCalPoint(float known_weight_g); // captures raw internally
void requestClearCalPoints();
void requestReloadParams();                    // re-reads NVS (config UI)

// Per-tick state-change event drained by the coordinator. Returns
// false on empty queue.
struct WeightEvent {
    WeightState new_state;
    float       weight_g;
};
bool pollEvent(WeightEvent& out);

// True if a calibration mutation just landed and the coordinator
// should push CalibrationStatus to the console. Read-and-clear.
bool consumeCalDirty();

}  // namespace SensorTask
