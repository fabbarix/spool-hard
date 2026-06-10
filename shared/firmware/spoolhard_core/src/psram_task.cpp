#include "spoolhard/psram_task.h"
#include <esp_heap_caps.h>

bool spoolhardSpawnPsramTask(TaskFunction_t fn, void* arg, const char* name,
                             size_t stack_bytes, UBaseType_t priority,
                             BaseType_t core_id, SpoolhardPsramTaskSlot& slot) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    // PSRAM first — the whole point is keeping internal DRAM free for
    // the allocations that have no choice (lwIP, DMA, task churn).
    if (!slot.busy) {
        if (slot.buf && slot.buf_bytes < stack_bytes) {
            heap_caps_free(slot.buf);
            slot.buf = nullptr;
        }
        if (!slot.buf) {
            slot.buf = (StackType_t*)heap_caps_malloc(
                stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            slot.buf_bytes = slot.buf ? stack_bytes : 0;
        }
        if (slot.buf) {
            slot.busy = true;
            TaskHandle_t h = xTaskCreateStaticPinnedToCore(
                fn, name, stack_bytes / sizeof(StackType_t), arg, priority,
                slot.buf, &slot.tcb, core_id);
            if (h) return true;
            slot.busy = false;
            Serial.printf("[Task %s] static PSRAM-stack create failed\n", name);
        }
    } else {
        Serial.printf("[Task %s] PSRAM slot busy — falling back to internal\n",
                      name);
    }
#else
    // FreeRTOS's portVALID_STACK_MEM assert PANICS on an external-RAM
    // stack unless the sdkconfig enables SPIRAM_ALLOW_STACK_EXTERNAL_-
    // MEMORY — and arduino-esp32's prebuilt qio_qspi config does not
    // (learned the hard way: console 0.12.10/11 panic-looped at the
    // boot+60s OTA check, crashes #2359-2363). Until the framework is
    // built with that option, every spawn takes the internal path.
    (void)slot;
#endif

    // Fallback: ordinary dynamic task on internal DRAM (also the only
    // path on PSRAM-less boards, where the SPIRAM alloc above fails).
    BaseType_t rc = xTaskCreatePinnedToCore(fn, name, stack_bytes, arg,
                                            priority, nullptr, core_id);
    if (rc != pdPASS) {
        Serial.printf("[Task %s] internal-stack create failed too\n", name);
        return false;
    }
    return true;
}
