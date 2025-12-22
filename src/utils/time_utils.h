#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Arduino-like millis(): milliseconds since boot.
// Uses ESP-IDF high-resolution timer and converts to ms.
static inline unsigned long millis() noexcept {
    return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

// Arduino-like delay(ms): sleep the current task for ms.
static inline void delay(unsigned long ms) noexcept {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
