#include "touch_driver.h"
#include "esp_log.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "driver/i2c_master.h"
#include "../config/logging.h"
#include "../utils/time_utils.h"

namespace {
constexpr char kTag[] = "TouchDriver";
// Persistent I2C bus and IO handles
static i2c_master_bus_handle_t i2c_bus_handle = nullptr;
static esp_lcd_panel_io_handle_t io_handle = nullptr;
}

// Redefine with new order to avoid 'esp_lcd_panel_io_i2c_config_t::control_phase_bytes' does not match declaration order in 'esp_lcd_panel_io_i2c_config_t'
#define ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()             \
    {                                                     \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, \
        .control_phase_bytes = 1,                         \
        .dc_bit_offset = 0,                               \
        .lcd_cmd_bits = 8,                                \
        .lcd_param_bits = 0,                              \
        .flags =                                          \
        {                                                 \
            .dc_low_on_data = 0,                          \
            .disable_control_phase = 1,                   \
        }                                                 \
    }

void TouchDriver::init() {
    if (initialized_) {
        return;
    }

    const TouchConfig& touch_config = ACTIVE_DISPLAY.touch;

    // Exit early if no touch controller configured
    if (touch_config.controller == TouchController::NONE) {
        disabled_ = true;
        LOG_BLE("[TOUCH] No touch controller configured, skipping initialization\n");
        return;
    }


    // Create I2C bus if not already created
    if (!i2c_bus_handle) {
        i2c_master_bus_config_t i2c_mst_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = (gpio_num_t)touch_config.sda_pin,
            .scl_io_num = (gpio_num_t)touch_config.scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to create I2C master bus: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
    }

    // Create IO handle if not already created
    if (!io_handle) {
        esp_lcd_panel_io_i2c_config_t io_config;
        if (touch_config.controller == TouchController::CST826) {
            io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
            io_config.scl_speed_hz = 100000;
        } else if (touch_config.controller == TouchController::FT3168) {
            io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        } else {
            LOG_BLE("[TOUCH] ERROR: Unknown touch controller type\n");
            disabled_ = true;
            return;
        }
        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_handle, &io_config, &io_handle);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to create I2C panel IO: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
    }

    esp_err_t ret = ESP_OK;
    if (touch_config.controller == TouchController::CST826) {
        esp_lcd_touch_config_t touch_cfg = {
            .x_max = ACTIVE_DISPLAY.width,
            .y_max = ACTIVE_DISPLAY.height,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,  // Polling mode
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        ret = esp_lcd_touch_new_i2c_cst816s(io_handle, &touch_cfg, &touch_handle_);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to initialize CST816S: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
        LOG_BLE("[TOUCH] CST816S initialized\n");
    } else if (touch_config.controller == TouchController::FT3168) {
        esp_lcd_touch_config_t touch_cfg = {
            .x_max = ACTIVE_DISPLAY.width,
            .y_max = ACTIVE_DISPLAY.height,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        ret = esp_lcd_touch_new_i2c_ft5x06(io_handle, &touch_cfg, &touch_handle_);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to initialize FT5x06: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
        LOG_BLE("[TOUCH] FT5x06 (FT3168) initialized\n");
    }

    // Select and configure the appropriate touch controller
    if (touch_config.controller == TouchController::CST826) {
        esp_lcd_touch_config_t touch_cfg = {
            .x_max = ACTIVE_DISPLAY.width,
            .y_max = ACTIVE_DISPLAY.height,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,  // Polling mode
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

        ret = esp_lcd_touch_new_i2c_cst816s(io_handle, &touch_cfg, &touch_handle_);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to initialize CST816S: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
        LOG_BLE("[TOUCH] CST816S initialized\n");

    } else if (touch_config.controller == TouchController::FT3168) {
        esp_lcd_touch_config_t touch_cfg = {
            .x_max = ACTIVE_DISPLAY.width,
            .y_max = ACTIVE_DISPLAY.height,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

        ret = esp_lcd_touch_new_i2c_ft5x06(io_handle, &touch_cfg, &touch_handle_);
        if (ret != ESP_OK) {
            LOG_BLE("[TOUCH] ERROR: Failed to initialize FT5x06: %s\n", esp_err_to_name(ret));
            disabled_ = true;
            return;
        }
        LOG_BLE("[TOUCH] FT5x06 (FT3168) initialized\n");

    } else {
        LOG_BLE("[TOUCH] ERROR: Unknown touch controller type\n");
        disabled_ = true;
        return;
    }

    initialized_ = true;
    disabled_ = false;
    last_touch_time_ = millis();
    LOG_BLE("[TOUCH] Touch driver initialized successfully\n");
}

void TouchDriver::deinit() {
    if (touch_handle_ != nullptr) {
        esp_lcd_touch_del(touch_handle_);
        touch_handle_ = nullptr;
    }
    initialized_ = false;
}

void TouchDriver::update() {
    if (!initialized_ || disabled_ || touch_handle_ == nullptr) {
        return;
    }

    // Read touch data using esp_lcd_touch library
    uint16_t touch_x[1], touch_y[1];
    uint16_t touch_strength[1];
    uint8_t touch_cnt = 0;

    esp_err_t ret = esp_lcd_touch_read_data(touch_handle_);
    if (ret != ESP_OK) {
        return;
    }

    // Get coordinates for up to 1 touch point
    bool touched = esp_lcd_touch_get_coordinates(touch_handle_, touch_x, touch_y, touch_strength, &touch_cnt, 1);

    if (touched && touch_cnt > 0) {
        last_touch_time_ = millis();
    }
}

void TouchDriver::disable() {
    disabled_ = true;
}

void TouchDriver::enable() {
    disabled_ = false;
}

uint32_t TouchDriver::get_ms_since_last_touch() const {
    if (!initialized_ || disabled_) {
        return 0;
    }
    return millis() - last_touch_time_;
}

