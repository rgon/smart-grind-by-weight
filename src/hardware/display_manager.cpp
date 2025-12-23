#include "display_manager.h"
#include "../config/constants.h"
#include "../config/logging.h"
#include <cstddef>
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal/lcd_types.h"
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

    // Check PSRAM availability once up front so we can choose buffer strategy
    size_t psram_size = esp_psram_get_size();
    if (psram_size == 0) {
        esp_err_t psram_err = esp_psram_init();
        if (psram_err == ESP_OK) {
            psram_size = esp_psram_get_size();
        }
    }
        psram_ok = psram_size > 0;
        LOG_BLE("[DISPLAY] PSRAM %s (size=%u bytes, free=%u)", psram_ok ? "OK" : "MISSING",
            (unsigned)psram_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Configure LVGL display buffers per official documentation:
    // Use partial buffer strategy with bounce buffer on RGB panel
    // Full buffer only used if PSRAM available AND not in bounce buffer mode
    uint32_t buff_size, trans_size;
    if (psram_ok) {
        // With PSRAM: use full screen buffer + DMA transfer region
        buff_size = screen_width * screen_height;  // Full screen in PSRAM
        trans_size = screen_width * 40;             // DMA transfer buffer in SRAM
    } else {
        // Without PSRAM: use partial buffer approach (like official example)
        buff_size = screen_width * 100;             // 100-line buffer in SRAM
        trans_size = 0;                             // No separate DMA buffer
    }

    // Add LCD display to LVGL
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = nullptr,
        .panel_handle = lcd_panel,
        .control_handle = nullptr,
        .buffer_size = buff_size,
        .double_buffer = false,    // Disable LVGL double buffer; RGB panel manages frame buffers
        .trans_size = trans_size,   // DMA transfer buffer when using PSRAM canvas
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
            .buff_dma = false,        // LVGL buffers in SRAM; RGB panel handles DMA
            .buff_spiram = psram_ok,  // Use PSRAM for canvas if available
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
            .avoid_tearing = psram_ok,  // only use panel framebuffers when PSRAM is available
        }
    };
    
    lvgl_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!lvgl_display) {
        LOG_BLE("[DISPLAY] ERROR: lvgl_port_add_disp_rgb returned NULL\n");
        return;
    }
    
    // Per official docs: All LVGL API calls must be protected with lvgl_port_lock/unlock
    lvgl_port_lock(0);
    
    // Apply round display clipping if needed
    if (config.is_round) {
        lv_obj_set_style_clip_corner(lv_scr_act(), true, 0);
    }
    
    lvgl_port_unlock();

    // Initialize physical touch driver
    touch_driver.init();
    
    // Register touch input with LVGL per official API
    lvgl_input = lv_indev_create();
    lv_indev_set_type(lvgl_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_disp(lvgl_input, lvgl_display);  // Associate input device with display
    lv_indev_set_read_cb(lvgl_input, touchpad_read_cb);

    // Mark as initialized before setting brightness (set_brightness checks initialized flag)
    initialized = true;

    // Ensure backlight is on by default after successful init
    set_brightness(1.0f);
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
    esp_lcd_rgb_panel_config_t panel_conf = {};
    panel_conf.clk_src = LCD_CLK_SRC_PLL160M;  // Match Espressif reference BSP
    panel_conf.timings.pclk_hz = 26 * 1000 * 1000;  // 20 MHz for UEDX48480021 | TODO: try 26
    panel_conf.timings.h_res = config.width;
    panel_conf.timings.v_res = config.height;
    panel_conf.timings.hsync_pulse_width = 8;
    panel_conf.timings.hsync_back_porch = 10;
    panel_conf.timings.hsync_front_porch = 50;
    panel_conf.timings.vsync_pulse_width = 2;
    panel_conf.timings.vsync_back_porch = 18;
    panel_conf.timings.vsync_front_porch = 8;
    panel_conf.timings.flags.hsync_idle_low = 1;  // Active-high pulse => idle low
    panel_conf.timings.flags.vsync_idle_low = 1;  // Active-high pulse => idle low
    panel_conf.timings.flags.de_idle_high = 0;
    panel_conf.timings.flags.pclk_active_neg = false;
    panel_conf.timings.flags.pclk_idle_high = 0;
    panel_conf.data_width = 16;
    // panel_conf.bits_per_pixel = 16;
    panel_conf.in_color_format = lcd_color_format_t::LCD_COLOR_FMT_RGB565;
    panel_conf.out_color_format = lcd_color_format_t::LCD_COLOR_FMT_RGB565;
    panel_conf.num_fbs = static_cast<size_t>(psram_ok ? 2 : 0);
    panel_conf.bounce_buffer_size_px = psram_ok ? (size_t)(config.width * 30) : (size_t)(config.width * 30);
    panel_conf.dma_burst_size = 64;
    panel_conf.hsync_gpio_num = (gpio_num_t)config.pins.hsync;
    panel_conf.vsync_gpio_num = (gpio_num_t)config.pins.vsync;
    panel_conf.de_gpio_num = (gpio_num_t)config.pins.de;
    panel_conf.pclk_gpio_num = (gpio_num_t)config.pins.pclk;
    panel_conf.disp_gpio_num = (gpio_num_t)(-1);
    panel_conf.data_gpio_nums[0] = (gpio_num_t)config.pins.b0;
    panel_conf.data_gpio_nums[1] = (gpio_num_t)config.pins.b1;
    panel_conf.data_gpio_nums[2] = (gpio_num_t)config.pins.b2;
    panel_conf.data_gpio_nums[3] = (gpio_num_t)config.pins.b3;
    panel_conf.data_gpio_nums[4] = (gpio_num_t)config.pins.b4;
    panel_conf.data_gpio_nums[5] = (gpio_num_t)config.pins.g0;
    panel_conf.data_gpio_nums[6] = (gpio_num_t)config.pins.g1;
    panel_conf.data_gpio_nums[7] = (gpio_num_t)config.pins.g2;
    panel_conf.data_gpio_nums[8] = (gpio_num_t)config.pins.g3;
    panel_conf.data_gpio_nums[9] = (gpio_num_t)config.pins.g4;
    panel_conf.data_gpio_nums[10] = (gpio_num_t)config.pins.g5;
    panel_conf.data_gpio_nums[11] = (gpio_num_t)config.pins.r0;
    panel_conf.data_gpio_nums[12] = (gpio_num_t)config.pins.r1;
    panel_conf.data_gpio_nums[13] = (gpio_num_t)config.pins.r2;
    panel_conf.data_gpio_nums[14] = (gpio_num_t)config.pins.r3;
    panel_conf.data_gpio_nums[15] = (gpio_num_t)config.pins.r4;
    // user_fbs is an array, leave it zero-initialized
    panel_conf.flags.disp_active_low = 0;
    panel_conf.flags.refresh_on_demand = 0;
    panel_conf.flags.fb_in_psram = static_cast<uint32_t>(psram_ok ? 1 : 0);
    panel_conf.flags.double_fb = 0;
    panel_conf.flags.no_fb = static_cast<uint32_t>(psram_ok ? 0 : 1);
    panel_conf.flags.bb_invalidate_cache = 0;
    
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
    {0x3A, (uint8_t[]){0x55}, 1, 0},  // Pixel format: 16-bit/pixel (RGB565)
    {0x29, (uint8_t[]){0x00}, 0, 0},  // Display on
};

esp_err_t DisplayManager::init_st7701_commands(const DisplayConfig& config) {
    LOG_BLE("[DISPLAY] Initializing ST7701 via 3-wire SPI...\n");
    
    // Reset LCD
    if (config.pins.rst >= 0) {
        LOG_BLE("[DISPLAY] Resetting LCD (pin %d)...\n", config.pins.rst);
        gpio_set_direction((gpio_num_t)config.pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)config.pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)config.pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)config.pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    
    // Configure 3-wire SPI for sending commands
    // Note: These pins (12, 13) are shared with RGB data lines and only used during init
    LOG_BLE("[DISPLAY] Configuring SPI bus: SCK=%d, MOSI=%d, CS=%d\n", 
            config.pins.sck, config.pins.sda, config.pins.cs);
    
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = (gpio_num_t)config.pins.sda;
    buscfg.miso_io_num = (gpio_num_t)(-1);
    buscfg.sclk_io_num = (gpio_num_t)config.pins.sck;
    buscfg.quadwp_io_num = (gpio_num_t)(-1);
    buscfg.quadhd_io_num = (gpio_num_t)(-1);
    buscfg.data4_io_num = -1;
    buscfg.data5_io_num = -1;
    buscfg.data6_io_num = -1;
    buscfg.data7_io_num = -1;
    buscfg.max_transfer_sz = 128;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER;
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_BLE("[DISPLAY] ERROR: spi_bus_initialize failed: 0x%X (%s)\n", ret, esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        LOG_BLE("[DISPLAY] WARNING: SPI bus already initialized (expected if reinitializing)\n");
    }
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = (gpio_num_t)config.pins.cs;
    io_config.dc_gpio_num = (gpio_num_t)(-1);  // 3-wire SPI (no DC pin)
    io_config.spi_mode = 0;
    io_config.pclk_hz = 2 * 1000 * 1000;  // 2 MHz for init commands (was 1 MHz)
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.lsb_first = 0;
    
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_new_panel_io_spi failed: 0x%X (%s)\n", ret, esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    
    LOG_BLE("[DISPLAY] Sending %d ST7701 init commands...\n", 
            sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]));
    
    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]); i++) {
        esp_lcd_panel_io_tx_param(io_handle, st7701_init_cmds[i].cmd, 
                                  st7701_init_cmds[i].data, st7701_init_cmds[i].data_bytes);
        if (st7701_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7701_init_cmds[i].delay_ms));
        }
    }
    
    LOG_BLE("[DISPLAY] ST7701 init commands sent successfully\n");
    
    // Clean up panel IO (we only need it for initialization)
    esp_lcd_panel_io_del(io_handle);
    spi_bus_free(SPI2_HOST);
    
    LOG_BLE("[DISPLAY] SPI bus freed, pins 12/13 now available for RGB data\n");
    
    return ESP_OK;
}

void DisplayManager::update() {
    if (!initialized) return;
    
    // Update touch driver (reads hardware state, no LVGL calls)
    touch_driver.update();
    
    // Note: lv_timer_handler() is called automatically by esp_lvgl_port's internal task
    // We should NOT call it manually to avoid race conditions
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
    if (!initialized) {
        LOG_BLE("[DISPLAY] WARNING: set_brightness called before initialized\n");
        return;
    }
    
    const DisplayConfig& config = ACTIVE_DISPLAY;
    
    LOG_BLE("[DISPLAY] set_brightness(%.2f) - driver: %s, bl_pin: %d, inverted: %d\n", 
            brightness, config.driver_name, config.pins.bl, config.inverted_backlight);
    
    // Clamp brightness to valid hardware range [0.0, 1.0]
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    
    // Apply minimum brightness constraint from config
    float min_brightness = config.min_brightness_pct / 100.0f;
    if (brightness < min_brightness) brightness = min_brightness;
    
    LOG_BLE("[DISPLAY] After clamp/min: brightness=%.2f\n", brightness);
    
    // Handle backlight inversion if configured
    if (config.inverted_backlight) {
        brightness = 1.0f - brightness;  // Invert: 1.0 becomes 0.0, 0.0 becomes 1.0
        LOG_BLE("[DISPLAY] After invert: brightness=%.2f\n", brightness);
    }
    
    // Set brightness based on driver type
    if (strcmp(config.driver_name, "ST7701") == 0) {
        // ST7701: Use PWM-based backlight control
        if (config.pins.bl >= 0) {
            uint8_t brightness_value = (uint8_t)(brightness * 255.0f);
            LOG_BLE("[DISPLAY] Setting ST7701 backlight to duty=%u (0x%02X)\n", brightness_value, brightness_value);
            
            // Configure PWM for backlight pin if not already configured
            static bool bl_pwm_configured = false;
            if (!bl_pwm_configured) {
                LOG_BLE("[DISPLAY] Configuring LEDC PWM for backlight...\n");
                ledc_timer_config_t ledc_timer = {
                    .speed_mode = LEDC_LOW_SPEED_MODE,
                    .duty_resolution = LEDC_TIMER_8_BIT,
                    .timer_num = LEDC_TIMER_0,
                    .freq_hz = 5000,
                    .clk_cfg = LEDC_AUTO_CLK,
                    .deconfigure = false
                };
                esp_err_t timer_err = ledc_timer_config(&ledc_timer);
                if (timer_err != ESP_OK) {
                    LOG_BLE("[DISPLAY] ERROR: ledc_timer_config failed: 0x%X\n", timer_err);
                    return;
                }

                ledc_channel_config_t ledc_channel = {
                    .gpio_num = config.pins.bl,
                    .speed_mode = LEDC_LOW_SPEED_MODE,
                    .channel = LEDC_CHANNEL_0,
                    .intr_type = LEDC_INTR_DISABLE,
                    .timer_sel = LEDC_TIMER_0,
                    .duty = 0,
                    .hpoint = 0,
                    .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
                    .flags = {},
                    .deconfigure = false
                };
                esp_err_t chan_err = ledc_channel_config(&ledc_channel);
                if (chan_err != ESP_OK) {
                    LOG_BLE("[DISPLAY] ERROR: ledc_channel_config failed: 0x%X\n", chan_err);
                    return;
                }
                LOG_BLE("[DISPLAY] LEDC PWM configured successfully\n");
                bl_pwm_configured = true;
            }
            
            esp_err_t duty_err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness_value);
            if (duty_err != ESP_OK) {
                LOG_BLE("[DISPLAY] ERROR: ledc_set_duty failed: 0x%X\n", duty_err);
                return;
            }
            
            esp_err_t update_err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            if (update_err != ESP_OK) {
                LOG_BLE("[DISPLAY] ERROR: ledc_update_duty failed: 0x%X\n", update_err);
                return;
            }
            
            LOG_BLE("[DISPLAY] Backlight duty set successfully\n");
        } else {
            LOG_BLE("[DISPLAY] WARNING: Backlight pin not configured (bl=%d)\n", config.pins.bl);
        }
    }
    else if (strcmp(config.driver_name, "CO5300") == 0) {
        // CO5300: Not implemented with esp_lvgl_port
        LOG_BLE("[DISPLAY] WARNING: CO5300 brightness control not implemented\n");
    } else {
        LOG_BLE("[DISPLAY] WARNING: Unknown driver for brightness control: %s\n", config.driver_name);
    }
}
