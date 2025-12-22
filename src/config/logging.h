#pragma once

#include "esp_log.h"
#include <cstdio>

// Forward declaration to avoid circular dependency
class BluetoothManager;
extern BluetoothManager g_bluetooth_manager;


/**
 * ESP-IDF Native Logging Module
 * Replaces Arduino Serial-based logging with ESP-IDF esp_log
 * Provides the same macro interface as the original logging.h
 */

/* Default log tag for app logging */
#define GRIND_LOG_TAG "GRIND"

/* Core logging macros using ESP-IDF esp_log.h */
#define LOG_BLE(format, ...) ESP_LOGI(GRIND_LOG_TAG, format, ##__VA_ARGS__)

#define LOG_DEBUG_PRINTF(format, ...) ESP_LOGD(GRIND_LOG_TAG, format, ##__VA_ARGS__)
#define LOG_DEBUG_PRINTLN(str) ESP_LOGD(GRIND_LOG_TAG, "%s", str)
#define LOG_DEBUG_PRINT(str) ESP_LOGD(GRIND_LOG_TAG, "%s", str)

/* Conditional subsystem debug macros */
#if DEBUG_GRIND_CONTROLLER
#define LOG_GRIND_DEBUG(format, ...) ESP_LOGD("GRIND_CTRL", format, ##__VA_ARGS__)
#else
#define LOG_GRIND_DEBUG(format, ...)
#endif

#if DEBUG_LOAD_CELL
#define LOG_LOADCELL_DEBUG(format, ...) ESP_LOGD("LOADCELL", format, ##__VA_ARGS__)
#else
#define LOG_LOADCELL_DEBUG(format, ...)
#endif

#if DEBUG_UI_SYSTEM
#define LOG_UI_DEBUG(format, ...) ESP_LOGD("UI", format, ##__VA_ARGS__)
#else
#define LOG_UI_DEBUG(format, ...)
#endif

#if DEBUG_SERIAL_OUTPUT
#define LOG_SERIAL_DEBUG(format, ...) ESP_LOGD("SERIAL", format, ##__VA_ARGS__)
#else
#define LOG_SERIAL_DEBUG(format, ...)
#endif

#if DEBUG_BLE_COMMS
#define LOG_BLE_DEBUG(format, ...) ESP_LOGD("BLE_COMM", format, ##__VA_ARGS__)
#else
#define LOG_BLE_DEBUG(format, ...)
#endif

#if DEBUG_STATE_MACHINE
#define LOG_STATE_DEBUG(format, ...) ESP_LOGD("STATE", format, ##__VA_ARGS__)
#else
#define LOG_STATE_DEBUG(format, ...)
#endif

#if DEBUG_OTA_HANDLER
#define LOG_OTA_DEBUG(format, ...) ESP_LOGD("OTA", format, ##__VA_ARGS__)
#else
#define LOG_OTA_DEBUG(format, ...)
#endif

#if DEBUG_FILE_IO
#define LOG_FILE_DEBUG(format, ...) ESP_LOGD("FILE_IO", format, ##__VA_ARGS__)
#else
#define LOG_FILE_DEBUG(format, ...)
#endif

/* Additional convenience macros for common log levels */
#define LOG_ERROR(format, ...) ESP_LOGE(GRIND_LOG_TAG, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) ESP_LOGW(GRIND_LOG_TAG, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) ESP_LOGI(GRIND_LOG_TAG, format, ##__VA_ARGS__)

/**
 * Initialize logging subsystem
 * Call this early in app_main() before any other logging
 */
static inline void logging_init(void) {
    /* Set default log level to INFO, can be overridden via esp_log_level_set() */
    esp_log_level_set("*", ESP_LOG_INFO);
    
    /* Optional: Suppress verbose logs from specific components */
    esp_log_level_set("esp_littlefs", ESP_LOG_WARN);
    esp_log_level_set("nimble", ESP_LOG_INFO);
}

/**
 * Set log level for a specific tag at runtime
 * Example: logging_set_level("GRIND", ESP_LOG_DEBUG);
 */
static inline void logging_set_level(const char *tag, esp_log_level_t level) {
    esp_log_level_set(tag, level);
}
