#include "sensor_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include "spoolhard/serial_mirror.h"

// External — owned by main.cpp. The task takes exclusive write
// authority once spawned; everyone else reads via the API below.
extern LoadCell g_scale;

namespace SensorTask {

namespace {

// ── Command channel ──────────────────────────────────────────
enum class CmdKind : uint8_t {
    Tare,
    Calibrate,
    AddCalPoint,
    ClearCalPoints,
    ReloadParams,
};
struct Cmd {
    CmdKind kind;
    float   weight_g;     // unused for unit kinds
};

QueueHandle_t s_cmd_q   = nullptr;     // 8 entries — bounded
QueueHandle_t s_event_q = nullptr;     // 8 entries — drained by coord loop
TaskHandle_t  s_task    = nullptr;
SemaphoreHandle_t s_cal_mtx = nullptr; // protects CalibrationData snapshots

// ── Snapshot publish (Linux-style seqlock) ───────────────────
//
// The task is the only writer; it bumps seq to odd before writing
// fields, then to even after. Readers double-check seq stayed even
// across the read. Lock-free reads, no priority inversion.
std::atomic<uint32_t> s_snap_seq{0};
Snapshot              s_snap;          // only mutated by task

// ── Calibration-dirty flag ───────────────────────────────────
std::atomic<bool> s_cal_dirty{false};

// ── Task body ────────────────────────────────────────────────
void _publishSnapshot() {
    // odd → writers
    uint32_t s = s_snap_seq.fetch_add(1, std::memory_order_acquire) + 1;
    (void)s;
    s_snap.state     = g_scale.getState();
    s_snap.weight_g  = g_scale.getWeightG();
    s_snap.last_raw  = g_scale.getLastRaw();
    s_snap.precision = g_scale.params().precision;
    // even → done
    s_snap_seq.fetch_add(1, std::memory_order_release);
}

void _drainCommands() {
    Cmd c;
    while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
        switch (c.kind) {
            case CmdKind::Tare:
                Serial.println("[Sensor] tare");
                g_scale.tare();
                s_cal_dirty.store(true, std::memory_order_release);
                break;
            case CmdKind::Calibrate:
                Serial.printf("[Sensor] calibrate %.1fg\n", c.weight_g);
                g_scale.calibrate(c.weight_g);
                s_cal_dirty.store(true, std::memory_order_release);
                break;
            case CmdKind::AddCalPoint: {
                Serial.printf("[Sensor] add cal point %.1fg\n", c.weight_g);
                long raw = g_scale.captureRaw();
                g_scale.addCalPoint(c.weight_g, raw);
                s_cal_dirty.store(true, std::memory_order_release);
                break;
            }
            case CmdKind::ClearCalPoints:
                Serial.println("[Sensor] clear cal points");
                g_scale.clearCalPoints();
                s_cal_dirty.store(true, std::memory_order_release);
                break;
            case CmdKind::ReloadParams:
                Serial.println("[Sensor] reload params");
                g_scale.loadParams();
                break;
        }
    }
}

void _taskBody(void*) {
    Serial.println("[Sensor] task starting");
    WeightState last = g_scale.getState();

    // Inner loop: 100 Hz cap. The HX711 in 10 Hz mode produces samples
    // every ~100 ms anyway, so most ticks here are no-ops (is_ready()
    // returns false). Once we move to 80 Hz hardware mode in Phase E3
    // this naturally absorbs the higher rate without code change —
    // each is_ready() simply hits more often.
    for (;;) {
        _drainCommands();
        g_scale.update();   // non-blocking: at most one sample per call

        WeightState st = g_scale.getState();
        if (st != last) {
            WeightEvent e{ st, g_scale.getWeightG() };
            // Drop on full — the coordinator will pick up the latest
            // state next tick anyway via getState().
            xQueueSend(s_event_q, &e, 0);
            last = st;
        }
        _publishSnapshot();

        vTaskDelay(pdMS_TO_TICKS(10));   // 100 Hz polling cap
    }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────

void begin() {
    if (s_task) return;
    s_cmd_q   = xQueueCreate(8, sizeof(Cmd));
    s_event_q = xQueueCreate(8, sizeof(WeightEvent));
    s_cal_mtx = xSemaphoreCreateMutex();
    BaseType_t r = xTaskCreatePinnedToCore(_taskBody, "sensor_task",
                                           4 * 1024, nullptr, 4, &s_task, 1);
    if (r != pdPASS) {
        Serial.println("[Sensor] xTaskCreate failed");
        s_task = nullptr;
    }
}

Snapshot snapshot() {
    Snapshot s;
    // seqlock retry — at most a few iterations under contention.
    for (;;) {
        uint32_t s0 = s_snap_seq.load(std::memory_order_acquire);
        if (s0 & 1) { taskYIELD(); continue; }
        s = s_snap;
        uint32_t s1 = s_snap_seq.load(std::memory_order_acquire);
        if (s0 == s1) return s;
    }
}
WeightState getState()    { return snapshot().state; }
float       getWeightG()  { return snapshot().weight_g; }
long        getLastRaw()  { return snapshot().last_raw; }
int         getPrecision(){ return snapshot().precision; }

CalibrationData snapshotCal() {
    if (!s_cal_mtx) return {};
    CalibrationData out;
    if (xSemaphoreTake(s_cal_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = g_scale.cal();
        xSemaphoreGive(s_cal_mtx);
    }
    return out;
}

static void _post(const Cmd& c) {
    if (!s_cmd_q) return;
    if (xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(10)) != pdTRUE) {
        Serial.printf("[Sensor] command queue full — dropped kind=%u\n",
                      (unsigned)c.kind);
    }
}

void requestTare()                      { _post({CmdKind::Tare,           0.0f}); }
void requestCalibrate(float w)          { _post({CmdKind::Calibrate,      w}); }
void requestAddCalPoint(float w)        { _post({CmdKind::AddCalPoint,    w}); }
void requestClearCalPoints()            { _post({CmdKind::ClearCalPoints, 0.0f}); }
void requestReloadParams()              { _post({CmdKind::ReloadParams,   0.0f}); }

bool pollEvent(WeightEvent& out) {
    if (!s_event_q) return false;
    return xQueueReceive(s_event_q, &out, 0) == pdTRUE;
}

bool consumeCalDirty() {
    return s_cal_dirty.exchange(false, std::memory_order_acq_rel);
}

}  // namespace SensorTask
