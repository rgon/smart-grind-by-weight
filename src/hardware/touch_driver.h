#pragma once
#include <esp_lcd_touch.h>
#include "../config/constants.h"

/**
 * @brief TouchDriver wraps the esp_lcd_touch library to provide unified touch handling.
 * 
 * Supports multiple touch controllers (CST826, FT3168, etc.) through esp_lcd_touch.
 * The actual driver selection is based on ACTIVE_DISPLAY.touch.controller.
 */
class TouchDriver {
public:
    /**
     * @brief Initialize the touch driver based on display configuration.
     */
    void init();

    /**
     * @brief Deinitialize the touch driver (cleanup resources).
     */
    void deinit();

    /**
     * @brief Disable touch input (e.g., during OTA updates).
     */
    void disable();

    /**
     * @brief Re-enable touch input.
     */
    void enable();

    /**
     * @brief Update touch tracking (call periodically to track touch activity time).
     */
    void update();

    /**
     * @brief Get the underlying esp_lcd_touch handle.
     * @return Touch device handle, or nullptr if not initialized.
     */
    esp_lcd_touch_handle_t get_handle() const { return touch_handle_; }

    /**
     * @brief Check if touch is initialized.
     */
    bool is_initialized() const { return initialized_; }

    /**
     * @brief Check if touch is disabled.
     */
    bool is_disabled() const { return disabled_; }

    /**
     * @brief Get milliseconds elapsed since the last touch event.
     * @return Time in milliseconds, or 0 if touch is disabled or never touched.
     */
    uint32_t get_ms_since_last_touch() const;

private:
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    bool initialized_ = false;
    bool disabled_ = false;
    uint32_t last_touch_time_ = 0;  // Track last touch activity
};
