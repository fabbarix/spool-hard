#include "spoolhard/deferred_reboot.h"
#include <Arduino.h>

static void _rebootTask(void* arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP.restart();
}

void spoolhardDeferredReboot(uint32_t delay_ms) {
    if (xTaskCreate(_rebootTask, "reboot", 2048,
                    (void*)(uintptr_t)delay_ms, 1, nullptr) != pdPASS) {
        delay(delay_ms);
        ESP.restart();
    }
}
