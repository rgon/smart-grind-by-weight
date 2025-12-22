#pragma once

#include <esp_err.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include <stdio.h>

/**
 * Initialize ESP-IDF native LittleFS filesystem via VFS
 * Replaces Arduino LittleFS.begin()
 * 
 * @return true if initialization successful, false otherwise
 */
static inline bool littlefs_init(void) {
    const char *base_path = "/littlefs";
    
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
    };
    
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW("LITTLEFS", "LittleFS already mounted");
            return true;
        }
        ESP_LOGE("LITTLEFS", "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI("LITTLEFS", "LittleFS mounted successfully at %s", base_path);
    return true;
}

/**
 * Check if LittleFS is available and healthy
 */
static inline bool littlefs_available(void) {
    FILE *f = fopen("/littlefs/.test", "w");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}
