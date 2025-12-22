#include "display_manager.h"
#include "../config/constants.h"
#include "../config/logging.h"
#include <cstddef>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <cstring>

// Fallback for ESP-IDF versions that don't expose vendor init command helper
#if !defined(ESP_LCD_PANEL_VENDOR_INIT_CMD_T_DEFINED)
typedef struct {
    int cmd;                // LCD command opcode
    const void *data;       // Command parameters
    size_t data_bytes;      // Length of parameters
    unsigned int delay_ms;  // Post-command delay
} esp_lcd_panel_vendor_init_cmd_t;
#define ESP_LCD_PANEL_VENDOR_INIT_CMD_T_DEFINED 1
#endif

DisplayManager* g_display_manager = nullptr;

void DisplayManager::init() {
    g_display_manager = this;
    
    // Get active display config
    const DisplayConfig& config = ACTIVE_DISPLAY;

    // Initialize display hardware
    init_display_hardware(config);
    
    if (!lcd_panel) {
        LOG_BLE("[DISPLAY] ERROR: Failed to initialize LCD panel\n");
        return;
    }
    
    // Initialize the LCD panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    
    screen_width = config.width;
    screen_height = config.height;

    // Initialize LVGL port
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Calculate buffer size (partial buffer for rendering)
    uint32_t buff_size = screen_width * 40; // 40 lines buffer

    // Add LCD display to LVGL
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = nullptr,
        .panel_handle = lcd_panel,
        .control_handle = nullptr,
        .buffer_size = buff_size,
        .double_buffer = true,
        .trans_size = 0,
        .hres = screen_width,
        .vres = screen_height,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .full_refresh = 0,
            .direct_mode = 0,
        }
    };
    
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,  // Bounce buffer mode
            .avoid_tearing = true,
        }
    };
    
    lvgl_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    
    if (config.is_round) {
        lv_obj_set_style_clip_corner(lv_scr_act(), true, 0);
    }

    // Initialize touch
    touch_driver.init();
    lvgl_input = lv_indev_create();
    lv_indev_set_type(lvgl_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_input, touchpad_read_cb);
    
    initialized = true;
}

void DisplayManager::init_display_hardware(const DisplayConfig& config) {
    switch (config.interface) {
        case DisplayInterfaceType::QSPI:
            LOG_BLE("[DISPLAY] ERROR: QSPI display interface not implemented\n");
            lcd_panel = nullptr;
            break;
        case DisplayInterfaceType::MIPI_PARALLEL:
            init_rgb_display(config);
            break;
    }
}

void DisplayManager::init_rgb_display(const DisplayConfig& config) {
    // ST7701 requires vendor-specific initialization via 3-wire SPI before RGB panel
    if (strcmp(config.driver_name, "ST7701") == 0) {
        esp_err_t ret = init_st7701_commands(config);
        if (ret != ESP_OK) {
            LOG_BLE("[DISPLAY] ERROR: ST7701 initialization failed\n");
            lcd_panel = nullptr;
            return;
        }
    }
    
    // Configure RGB panel timing based on display profile
    esp_lcd_rgb_panel_config_t panel_conf = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,  // 16 MHz for UEDX48480021
            .h_res = config.width,
            .v_res = config.height,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 20,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 50,
            .flags = {
                .hsync_idle_low = 0,
                .vsync_idle_low = 0,
                .de_idle_high = 0,
                .pclk_active_neg = false,
                .pclk_idle_high = 0,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = config.width * 10,
        .sram_trans_align = 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = config.pins.hsync,
        .vsync_gpio_num = config.pins.vsync,
        .de_gpio_num = config.pins.de,
        .pclk_gpio_num = config.pins.pclk,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            config.pins.b0, config.pins.b1, config.pins.b2, config.pins.b3, config.pins.b4,
            config.pins.g0, config.pins.g1, config.pins.g2, config.pins.g3, config.pins.g4, config.pins.g5,
            config.pins.r0, config.pins.r1, config.pins.r2, config.pins.r3, config.pins.r4,
        },
        .flags = {
            .disp_active_low = 0,
            .refresh_on_demand = 0,
            .fb_in_psram = 1,
            .double_fb = 0,
            .no_fb = 0,
            .bb_invalidate_cache = 0,
        },
    };
    
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_conf, &lcd_panel));
}

// ST7701 vendor-specific initialization commands
// Based on UEDX48480021-MD80ET.h for round display
static const esp_lcd_panel_vendor_init_cmd_t st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0B, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x07, 0x02}, 2, 0},
    {0xC7, (uint8_t[]){0x00}, 1, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xCD, (uint8_t[]){0x08}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x6D}, 1, 0},
    {0xB1, (uint8_t[]){0x37}, 1, 0},
    {0xB2, (uint8_t[]){0x8B}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x43}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x20}, 1, 0},
    {0xC0, (uint8_t[]){0x09}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}, 11, 0},
    {0xE2, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x00}, 4, 0},
    {0xE4, (uint8_t[]){0x22, 0x00}, 2, 0},
    {0xE5, (uint8_t[]){0x05, 0xEC, 0xF6, 0xCA, 0x07, 0xEE, 0xF6, 0xCA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x00}, 4, 0},
    {0xE7, (uint8_t[]){0x22, 0x00}, 2, 0},
    {0xE8, (uint8_t[]){0x06, 0xED, 0xF6, 0xCA, 0x08, 0xEF, 0xF6, 0xCA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16, 0},
    {0xE9, (uint8_t[]){0x36, 0x00}, 2, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}, 7, 0},
    {0xED, (uint8_t[]){0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xFF, 0x45, 0xFF, 0xFF, 0x54, 0xFF, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t[]){0x00, 0x0E}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},  // Sleep out + 120ms delay
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t[]){0x00, 0x0C}, 2, 0},
    {0xE8, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},  // Memory access control
    {0x3A, (uint8_t[]){0x66}, 1, 0},  // Pixel format: 18-bit/pixel
    {0x29, (uint8_t[]){0x00}, 0, 0},  // Display on
};

esp_err_t DisplayManager::init_st7701_commands(const DisplayConfig& config) {
    // Reset LCD
    if (config.pins.rst >= 0) {
        gpio_set_direction((gpio_num_t)config.pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)config.pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)config.pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)config.pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    
    // Configure 3-wire SPI for sending commands
    spi_bus_config_t buscfg = {
        .mosi_io_num = config.pins.sda,
        .miso_io_num = -1,
        .sclk_io_num = config.pins.sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = config.pins.cs,
        .dc_gpio_num = -1,  // 3-wire SPI (no DC pin)
        .spi_mode = 0,
        .pclk_hz = 1 * 1000 * 1000,  // 1 MHz for init commands
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));
    
    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]); i++) {
        esp_lcd_panel_io_tx_param(io_handle, st7701_init_cmds[i].cmd, 
                                  st7701_init_cmds[i].data, st7701_init_cmds[i].data_bytes);
        if (st7701_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7701_init_cmds[i].delay_ms));
        }
    }
    
    // Clean up panel IO (we only need it for initialization)
    esp_lcd_panel_io_del(io_handle);
    spi_bus_free(SPI2_HOST);
    
    return ESP_OK;
}

void DisplayManager::update() {
    if (!initialized) return;
    
    touch_driver.update();
    lv_timer_handler();
}

void DisplayManager::touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (!g_display_manager) return;
    
    TouchData touch = g_display_manager->touch_driver.get_touch_data();
    
    if (touch.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touch.x;
        data->point.y = touch.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void DisplayManager::set_brightness(float brightness) {
    if (!initialized) return;
    
    const DisplayConfig& config = ACTIVE_DISPLAY;
    
    // Clamp brightness to valid hardware range [0.0, 1.0]
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    
    // Apply minimum brightness constraint from config
    float min_brightness = config.min_brightness_pct / 100.0f;
    if (brightness < min_brightness) brightness = min_brightness;
    
    // Set brightness based on driver type
    if (strcmp(config.driver_name, "ST7701") == 0) {
        // ST7701: Use PWM-based backlight control
        if (config.pins.bl >= 0) {
            uint8_t brightness_value = (uint8_t)(brightness * 255.0f);
            // Configure PWM for backlight pin if not already configured
            static bool bl_pwm_configured = false;
            if (!bl_pwm_configured) {
                ledc_timer_config_t ledc_timer = {
                    .speed_mode = LEDC_LOW_SPEED_MODE,
                    .duty_resolution = LEDC_TIMER_8_BIT,
                    .timer_num = LEDC_TIMER_0,
                    .freq_hz = 5000,
                    .clk_cfg = LEDC_AUTO_CLK
                };
                ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

                ledc_channel_config_t ledc_channel = {
                    .gpio_num = config.pins.bl,
                    .speed_mode = LEDC_LOW_SPEED_MODE,
                    .channel = LEDC_CHANNEL_0,
                    .intr_type = LEDC_INTR_DISABLE,
                    .timer_sel = LEDC_TIMER_0,
                    .duty = 0,
                    .hpoint = 0
                };
                ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
                bl_pwm_configured = true;
            }
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness_value));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        }
    }
    else if (strcmp(config.driver_name, "CO5300") == 0) {
        // CO5300: Not implemented with esp_lvgl_port
        LOG_BLE("[DISPLAY] WARNING: CO5300 brightness control not implemented\n");
    }
}
