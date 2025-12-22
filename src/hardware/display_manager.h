#pragma once
#include <lvgl.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lvgl_port.h"
#include "touch_driver.h"
#include "../config/constants.h"

class DisplayManager {
private:
    esp_lcd_panel_handle_t lcd_panel;
    lv_display_t* lvgl_display;
    lv_indev_t* lvgl_input;
    TouchDriver touch_driver;
    
    uint32_t screen_width;
    uint32_t screen_height;
    bool initialized;

public:
    void init();
    void update();
    void set_brightness(float brightness);
    
    uint32_t get_width() const { return screen_width; }
    uint32_t get_height() const { return screen_height; }
    bool is_initialized() const { return initialized; }
    TouchDriver* get_touch_driver() { return &touch_driver; }
    
private:
    static void touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

    void init_display_hardware(const DisplayConfig& config);
    void init_rgb_display(const DisplayConfig& config);
    esp_err_t init_st7701_commands(const DisplayConfig& config);
};

extern DisplayManager* g_display_manager;
