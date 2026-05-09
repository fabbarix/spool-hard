#include "console_tx.h"
#include "console_channel.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include "spoolhard/serial_mirror.h"

namespace ConsoleTx {

namespace {

// Queue holds pointers to heap-allocated Strings. A pointer fits in
// xQueueSend's payload (4 bytes); the String itself isn't moved
// through the queue because String copy/destroy semantics under
// arduino-esp32 aren't perfectly trivial-by-bytes.
QueueHandle_t s_q     = nullptr;
TaskHandle_t  s_task  = nullptr;
constexpr size_t kQueueDepth = 32;

std::atomic<uint32_t> s_tx{0};
std::atomic<uint32_t> s_dropped{0};

void _taskBody(void*) {
    Serial.println("[ConsoleTx] task starting");
    String* item = nullptr;
    for (;;) {
        // Block indefinitely until something arrives. Cheap — task is
        // not woken when idle.
        if (xQueueReceive(s_q, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!item) continue;
        g_console.sendText(*item);
        s_tx.fetch_add(1, std::memory_order_relaxed);
        delete item;
    }
}

}  // namespace

void begin() {
    if (s_task) return;
    s_q = xQueueCreate(kQueueDepth, sizeof(String*));
    BaseType_t r = xTaskCreatePinnedToCore(_taskBody, "console_tx",
                                           4 * 1024, nullptr, 4, &s_task, 1);
    if (r != pdPASS) {
        Serial.println("[ConsoleTx] xTaskCreate failed");
        s_task = nullptr;
    }
}

bool send(const String& frame) {
    if (!s_q) return false;
    auto* p = new String(frame);
    if (xQueueSend(s_q, &p, pdMS_TO_TICKS(10)) != pdTRUE) {
        delete p;
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

size_t free() {
    if (!s_q) return 0;
    return uxQueueSpacesAvailable(s_q);
}
size_t total() { return kQueueDepth; }
uint32_t framesTx()      { return s_tx.load(std::memory_order_relaxed); }
uint32_t framesDropped() { return s_dropped.load(std::memory_order_relaxed); }

}  // namespace ConsoleTx
