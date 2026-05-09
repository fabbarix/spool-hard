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

std::atomic<uint32_t> s_snap_seq{0};
Snapshot              s_snap;

void _publishSnapshot() {
    s_snap_seq.fetch_add(1, std::memory_order_acquire);
    s_snap.status = g_nfc.getStatus();
    s_snap.tag    = g_nfc.getLastTag();
    s_snap_seq.fetch_add(1, std::memory_order_release);
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
            StatusEvent e{ st, g_nfc.getLastTag() };
            xQueueSend(s_event_q, &e, 0);
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
    s_event_q = xQueueCreate(8, sizeof(StatusEvent));
    BaseType_t r = xTaskCreatePinnedToCore(_taskBody, "nfc_task",
                                           4 * 1024, nullptr, 4, &s_task, 1);
    if (r != pdPASS) {
        Serial.println("[NFC] xTaskCreate failed");
        s_task = nullptr;
    }
}

Snapshot snapshot() {
    Snapshot s;
    for (;;) {
        uint32_t s0 = s_snap_seq.load(std::memory_order_acquire);
        if (s0 & 1) { taskYIELD(); continue; }
        s = s_snap;
        uint32_t s1 = s_snap_seq.load(std::memory_order_acquire);
        if (s0 == s1) return s;
    }
}
TagStatus getStatus() { return snapshot().status; }

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
    return xQueueReceive(s_event_q, &out, 0) == pdTRUE;
}

}  // namespace NfcTask
