#include "nfc_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include "spoolhard/serial_mirror.h"

extern NfcReader g_nfc;

namespace NfcTask {

namespace {

enum class CmdKind : uint8_t { Write, Erase, Emulate };
struct Cmd {
    CmdKind kind;
    // Variant payload — only one of these is meaningful per kind.
    WriteRequest write;
    EraseRequest erase;
    String       emulate_url;
};

QueueHandle_t s_cmd_q   = nullptr;
QueueHandle_t s_event_q = nullptr;
TaskHandle_t  s_task    = nullptr;

std::atomic<TagStatus> s_status{TagStatus::Idle};   // lock-free, POD
SemaphoreHandle_t      s_tag_mtx = nullptr;         // guards s_snap_tag
SpoolTag               s_snap_tag;
uint32_t               s_snap_gen = 0xFFFFFFFFu;    // task-private

void _publishSnapshot() {
    // Tag first, status last: a reader that sees a new status via
    // snapshot() then never pairs it with an older tag.
    // Tag holds Strings: copy under the mutex, and only when it changed.
    uint32_t gen = g_nfc.tagGeneration();
    if (gen != s_snap_gen && s_tag_mtx &&
        xSemaphoreTake(s_tag_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snap_tag = g_nfc.getLastTag();
        xSemaphoreGive(s_tag_mtx);
        s_snap_gen = gen;
    }
    s_status.store(g_nfc.getStatus(), std::memory_order_release);
}

void _drainCommands() {
    Cmd* c = nullptr;   // Cmd contains String — heap-allocate to avoid
                        // copying String objects through the queue.
    while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE && c) {
        switch (c->kind) {
            case CmdKind::Write:
                Serial.printf("[NFC] write tag (uid_len=%u)\n",
                              (unsigned)c->write.uid_len);
                g_nfc.writeTag(c->write.uid, c->write.uid_len,
                               c->write.ndef_message, c->write.cookie);
                break;
            case CmdKind::Erase:
                Serial.printf("[NFC] erase tag (uid_len=%u)\n",
                              (unsigned)c->erase.uid_len);
                g_nfc.eraseTag(c->erase.uid, c->erase.uid_len);
                break;
            case CmdKind::Emulate:
                Serial.printf("[NFC] emulate %s\n", c->emulate_url.c_str());
                g_nfc.emulateTag(c->emulate_url);
                break;
        }
        delete c;
        c = nullptr;
    }
}

void _taskBody(void*) {
    Serial.println("[NFC] task starting");
    TagStatus last = g_nfc.getStatus();

    for (;;) {
        _drainCommands();
        g_nfc.update();   // 30 ms PN532 poll inside

        TagStatus st = g_nfc.getStatus();
        if (st != last) {
            // StatusEvent holds Strings — never memcpy it through the
            // queue. Heap-allocate and queue the pointer.
            auto* e = new StatusEvent{ st, g_nfc.getLastTag() };
            if (xQueueSend(s_event_q, &e, 0) != pdTRUE) {
                Serial.println("[NFC] event queue full — dropped");
                delete e;
            }
            last = st;
        }
        _publishSnapshot();

        // PN532 RF poll cycle is ~30 ms inside g_nfc.update(). Leave
        // a small idle gap so the task doesn't hog the CPU when no
        // tag is present (most of the time).
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────

void begin() {
    if (s_task) return;
    s_cmd_q   = xQueueCreate(4, sizeof(Cmd*));      // pointers — Cmd contains Strings
    s_event_q = xQueueCreate(8, sizeof(StatusEvent*));   // pointers — Strings inside
    s_tag_mtx = xSemaphoreCreateMutex();
    BaseType_t r = xTaskCreatePinnedToCore(_taskBody, "nfc_task",
                                           4 * 1024, nullptr, 4, &s_task, 1);
    if (r != pdPASS) {
        Serial.println("[NFC] xTaskCreate failed");
        s_task = nullptr;
    }
}

Snapshot snapshot() {
    Snapshot s;
    s.status = s_status.load(std::memory_order_acquire);
    if (s_tag_mtx && xSemaphoreTake(s_tag_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s.tag = s_snap_tag;
        xSemaphoreGive(s_tag_mtx);
    }
    return s;
}
TagStatus getStatus() { return s_status.load(std::memory_order_acquire); }

static void _post(Cmd* c) {
    if (!s_cmd_q) { delete c; return; }
    if (xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(10)) != pdTRUE) {
        Serial.println("[NFC] command queue full — dropped");
        delete c;
    }
}

void requestWrite(const WriteRequest& r) {
    auto* c = new Cmd{};
    c->kind  = CmdKind::Write;
    c->write = r;
    _post(c);
}
void requestErase(const EraseRequest& r) {
    auto* c = new Cmd{};
    c->kind  = CmdKind::Erase;
    c->erase = r;
    _post(c);
}
void requestEmulate(const String& url) {
    auto* c = new Cmd{};
    c->kind        = CmdKind::Emulate;
    c->emulate_url = url;
    _post(c);
}

bool pollEvent(StatusEvent& out) {
    if (!s_event_q) return false;
    StatusEvent* e = nullptr;
    if (xQueueReceive(s_event_q, &e, 0) != pdTRUE || !e) return false;
    out = *e;
    delete e;
    return true;
}

}  // namespace NfcTask
