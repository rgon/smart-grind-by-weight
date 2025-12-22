#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include "nvs.h"
#include "nvs_flash.h"

// Minimal Preferences wrapper compatible with Arduino-style API
// Implements subset used by this project on ESP-IDF via NVS
class Preferences {
public:
    Preferences() : handle_(0), open_(false) {}
    ~Preferences() { end(); }

    bool begin(const char* ns, bool readOnly) {
        if (open_) end();
        // Ensure NVS initialized once
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err != ESP_OK) return false;
        err = nvs_open(ns, readOnly ? NVS_READONLY : NVS_READWRITE, &handle_);
        open_ = (err == ESP_OK);
        return open_;
    }

    void end() {
        if (open_) {
            nvs_close(handle_);
            open_ = false;
            handle_ = 0;
        }
    }

    int getInt(const char* key, int defaultVal) const {
        int32_t v = 0;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_i32(handle_, key, &v);
        return (err == ESP_OK) ? static_cast<int>(v) : defaultVal;
    }

    uint32_t getUInt(const char* key, uint32_t defaultVal) const {
        uint32_t v = 0;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_u32(handle_, key, &v);
        return (err == ESP_OK) ? v : defaultVal;
    }

    float getFloat(const char* key, float defaultVal) const {
        if (!open_) return defaultVal;
        size_t len = sizeof(float);
        float v = 0.0f;
        esp_err_t err = nvs_get_blob(handle_, key, &v, &len);
        return (err == ESP_OK && len == sizeof(float)) ? v : defaultVal;
    }

    uint64_t getULong64(const char* key, uint64_t defaultVal) const {
        uint64_t v = 0;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_u64(handle_, key, &v);
        return (err == ESP_OK) ? v : defaultVal;
    }

    // Add missing Arduino Preferences methods as stubs/compatibility
    unsigned char getUChar(const char* key, unsigned char defaultVal) const {
        uint8_t v = defaultVal;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_u8(handle_, key, &v);
        return (err == ESP_OK) ? v : defaultVal;
    }

    char getChar(const char* key, char defaultVal) const {
        int8_t v = static_cast<int8_t>(defaultVal);
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_i8(handle_, key, &v);
        return (err == ESP_OK) ? static_cast<char>(v) : defaultVal;
    }

    unsigned short getUShort(const char* key, unsigned short defaultVal) const {
        uint16_t v = defaultVal;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_u16(handle_, key, &v);
        return (err == ESP_OK) ? v : defaultVal;
    }

    short getShort(const char* key, short defaultVal) const {
        int16_t v = static_cast<int16_t>(defaultVal);
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_i16(handle_, key, &v);
        return (err == ESP_OK) ? static_cast<short>(v) : defaultVal;
    }

    long getLong64(const char* key, long defaultVal) const {
        // Alias for getULong64 for compatibility
        return static_cast<long>(getULong64(key, static_cast<uint64_t>(defaultVal)));
    }

    double getDouble(const char* key, double defaultVal) const {
        if (!open_) return defaultVal;
        size_t len = sizeof(double);
        double v = 0.0;
        esp_err_t err = nvs_get_blob(handle_, key, &v, &len);
        return (err == ESP_OK && len == sizeof(double)) ? v : defaultVal;
    }

    bool getBool(const char* key, bool defaultVal) const {
        uint8_t v = defaultVal ? 1 : 0;
        if (!open_) return defaultVal;
        esp_err_t err = nvs_get_u8(handle_, key, &v);
        if (err == ESP_OK) return v != 0;
        return defaultVal;
    }

    bool putInt(const char* key, int value) {
        if (!open_) return false;
        esp_err_t err = nvs_set_i32(handle_, key, static_cast<int32_t>(value));
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool putUInt(const char* key, uint32_t value) {
        if (!open_) return false;
        esp_err_t err = nvs_set_u32(handle_, key, value);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool putFloat(const char* key, float value) {
        if (!open_) return false;
        esp_err_t err = nvs_set_blob(handle_, key, &value, sizeof(value));
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    // Binary blob helpers
    size_t getBytesLength(const char* key) const {
        if (!open_) return 0;
        size_t len = 0;
        esp_err_t err = nvs_get_blob(handle_, key, nullptr, &len);
        return (err == ESP_OK) ? len : 0;
    }

    size_t getBytes(const char* key, void* buf, size_t maxLen) const {
        if (!open_ || !buf || maxLen == 0) return 0;
        size_t len = maxLen;
        esp_err_t err = nvs_get_blob(handle_, key, buf, &len);
        return (err == ESP_OK) ? len : 0;
    }

    bool putBytes(const char* key, const void* data, size_t len) {
        if (!open_ || (!data && len > 0)) return false;
        esp_err_t err = nvs_set_blob(handle_, key, data, len);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool putULong64(const char* key, uint64_t value) {
        if (!open_) return false;
        esp_err_t err = nvs_set_u64(handle_, key, value);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool putBool(const char* key, bool value) {
        if (!open_) return false;
        esp_err_t err = nvs_set_u8(handle_, key, value ? 1 : 0);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool remove(const char* key) {
        if (!open_) return false;
        esp_err_t err = nvs_erase_key(handle_, key);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    bool clear() {
        if (!open_) return false;
        esp_err_t err = nvs_erase_all(handle_);
        if (err != ESP_OK) return false;
        return nvs_commit(handle_) == ESP_OK;
    }

    // String helpers (stored as NVS string)
    std::string getString(const char* key, const char* defaultVal = "") const {
        if (!open_) return std::string(defaultVal);
        size_t len = 0;
        esp_err_t err = nvs_get_str(handle_, key, nullptr, &len);
        if (err != ESP_OK || len == 0) {
            return std::string(defaultVal);
        }
        std::string out;
        out.resize(len); // includes null terminator length
        err = nvs_get_str(handle_, key, out.data(), &len);
        if (err != ESP_OK) {
            return std::string(defaultVal);
        }
        if (!out.empty() && out.back() == '\0') {
            out.pop_back();
        }
        return out;
    }

    size_t putString(const char* key, const char* value) {
        if (!open_ || !key || !value) return 0;
        esp_err_t err = nvs_set_str(handle_, key, value);
        if (err != ESP_OK) return 0;
        if (nvs_commit(handle_) != ESP_OK) return 0;
        return strlen(value);
    }

    bool isKey(const char* key) const {
        if (!open_) return false;
        // Try common types to detect existence
        size_t len = 0;
        esp_err_t err = nvs_get_blob(handle_, key, nullptr, &len);
        if (err == ESP_OK) return true;
        uint64_t u64;
        if (nvs_get_u64(handle_, key, &u64) == ESP_OK) return true;
        uint32_t u32;
        if (nvs_get_u32(handle_, key, &u32) == ESP_OK) return true;
        int32_t i32;
        if (nvs_get_i32(handle_, key, &i32) == ESP_OK) return true;
        uint8_t u8;
        if (nvs_get_u8(handle_, key, &u8) == ESP_OK) return true;
        // Attempt string
        size_t strlen_required = 0;
        if (nvs_get_str(handle_, key, nullptr, &strlen_required) == ESP_OK) return true;
        return false;
    }

private:
    nvs_handle_t handle_;
    bool open_;
};
