#include "housekeeping.h"
#include "sdcard.h"
#include "crash_logger.h"
#include "spoolhard/ring_log.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "spoolhard/serial_mirror.h"

// Stack: 5120 B internal (4096 measured ~1.1 KB free; SD.begin remount path unexercised). Worst path is SdCard::_tryMount() (SD.begin ->
// sdmmc/FATFS mount, ~2 KB) or CrashLogger::flush() (260 B line buffer + File
// + snprintf; the entry vector lives on the heap). The two never nest. The
// high-water mark is logged ("[HK] stack_hwm") at +30 s and then every 5 min
// so a verifier can confirm headroom; bump kStack if it drops below ~1 KB.
static constexpr uint32_t kStack  = 5120;
static constexpr uint32_t kTickMs = 100;
static std::atomic<bool> s_active{false};

static void _hkTask(void*) {
    uint32_t lastHwm = millis();
    bool first = true;
    for (;;) {
        g_sd.update();
        CrashLogger::update();
        uint32_t now = millis();
        if (now - lastHwm >= (first ? 30000UL : 300000UL)) {
            first = false;
            lastHwm = now;
            dlog("HK", "stack_hwm=%u B free", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

void housekeepingBegin() {
    if (s_active.load()) return;
    if (xTaskCreatePinnedToCore(_hkTask, "housekeeping", kStack, nullptr, 1,
                                nullptr, 0) == pdPASS) {
        s_active.store(true);
    } else {
        Serial.println("[HK] task create failed - loopTask keeps SD/crash-log duties");
    }
}

bool housekeepingActive() { return s_active.load(); }
