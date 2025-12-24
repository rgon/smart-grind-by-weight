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
#include "esp_cache.h"
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

    
    // Optional: draw a small test pattern before LVGL to verify panel output
    // initialized = true;
    // draw_test_pattern();
    // set_brightness(0.2f);
    // vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Initialize LVGL port
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        // .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
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

    // Print: adding display to lvgl with resolution WxH
    LOG_BLE("[DISPLAY] Adding LVGL display: %dx%d, buffer size=%u pixels, sizeof(lv_color_t)=%u\n",
            screen_width, screen_height, (unsigned)buff_size, (unsigned)sizeof(lv_color_t));

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
            .buff_spiram = psram_ok,     // Use PSRAM for large buffers
            .sw_rotate = false,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .full_refresh = 0,
            .direct_mode = 0
        }
    };
    
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,  // Bounce buffer mode
            .avoid_tearing = false,  // psram_ok // Disable tearing avoidance (requires 2 FBs)
        }
    };
    
    lvgl_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!lvgl_display) {
        LOG_BLE("[DISPLAY] ERROR: lvgl_port_add_disp_rgb returned NULL\n");
        return;
    }
    
    // Initialize physical touch driver using esp_lcd_touch library
    touch_driver.init();

    // Add touch input (for selected screen) AFTER touch driver is initialized
    if (lvgl_display && touch_driver.get_handle() != nullptr) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lvgl_display,
            .handle = touch_driver.get_handle(),
        };
        lv_indev_t* touch_handle = lvgl_port_add_touch(&touch_cfg);
        // /* If deinitializing LVGL port, remember to delete all touches: */
        // lvgl_port_remove_touch(touch_handle);
        LOG_BLE("[DISPLAY] Touch device registered with LVGL\n");
    } else {
        LOG_BLE("[DISPLAY] Touch driver not available, touch input disabled\n");
    }

    // Per official docs: All LVGL API calls must be protected with lvgl_port_lock/unlock
    // lvgl_port_lock(0);
    // Apply round display clipping if needed
    // if (config.is_round) {
    //     lv_obj_set_style_clip_corner(lv_scr_act(), true, 0);
    // }
    // lvgl_port_unlock();

    // Turn on backlight after LVGL init (vendor does this in app_main after UI init)
    if (config.pins.bl >= 0) {
        LOG_BLE("[DISPLAY] Turning on backlight (pin %d = LOW)\n", config.pins.bl);
        gpio_set_level((gpio_num_t)config.pins.bl, 0);  // LOW = on for inverted backlight
    }

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
    esp_lcd_rgb_panel_config_t panel_conf = {};
    panel_conf.clk_src = LCD_CLK_SRC_PLL160M;  // Match Espressif reference BSP
    panel_conf.timings.pclk_hz = 18 * 1000 * 1000;  // 18MHz for touch version per vendor bsp.c
    panel_conf.timings.h_res = config.width;
    panel_conf.timings.v_res = config.height;
    panel_conf.timings.hsync_pulse_width = 8;
    panel_conf.timings.hsync_back_porch = 20;
    panel_conf.timings.hsync_front_porch = 40;
    panel_conf.timings.vsync_pulse_width = 8;
    panel_conf.timings.vsync_back_porch = 20;
    panel_conf.timings.vsync_front_porch = 50;
    panel_conf.timings.flags.hsync_idle_low = 0;  // Active-high pulse => idle low
    panel_conf.timings.flags.vsync_idle_low = 0;  // Active-high pulse => idle low
    panel_conf.timings.flags.de_idle_high = 0;
    panel_conf.timings.flags.pclk_active_neg = false;
    panel_conf.timings.flags.pclk_idle_high = 0;
    panel_conf.data_width = 16;
    // panel_conf.bits_per_pixel = 16;  // Not available in all ESP-IDF versions
    panel_conf.in_color_format = lcd_color_format_t::LCD_COLOR_FMT_RGB565;
    panel_conf.out_color_format = lcd_color_format_t::LCD_COLOR_FMT_RGB565;
    panel_conf.num_fbs = static_cast<size_t>(psram_ok ? 1 : 0); // Use 1 FB to prevent flashing issues
    panel_conf.bounce_buffer_size_px = (size_t)(config.width * 10);  // Align with vendor reference to avoid tearing
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
    // panel_conf.flags.disp_active_low = 0;
    // panel_conf.flags.refresh_on_demand = 0;
    panel_conf.flags.fb_in_psram = static_cast<uint32_t>(psram_ok ? 1 : 0);
    panel_conf.flags.double_fb = 0;
    panel_conf.flags.no_fb = static_cast<uint32_t>(psram_ok ? 0 : 1);
    // panel_conf.flags.bb_invalidate_cache = 0;
    
    LOG_BLE("[DISPLAY] Creating RGB panel: %dx%d, %.1fMHz PCLK, %d FBs\n",
            config.width, config.height, panel_conf.timings.pclk_hz / 1000000.0f,
            (int)panel_conf.num_fbs);
    
    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_conf, &lcd_panel);
    if (ret != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_new_rgb_panel failed: 0x%X (%s)\n", ret, esp_err_to_name(ret));
        lcd_panel = nullptr;
        return;
    }
    
    LOG_BLE("[DISPLAY] RGB panel created successfully\n");
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
    {0x3A, (uint8_t[]){0x77}, 1, 0},  // Pixel format: 0x77 per vendor bsp.c
    {0x29, (uint8_t[]){0x00}, 0, 20},  // Display on + 20ms delay for panel to stabilize
};

// Helper for bit-banged SPI delay
static inline void udelay(uint32_t us) {
    esp_rom_delay_us(us);
}

esp_err_t DisplayManager::init_st7701_commands(const DisplayConfig& config) {
    LOG_BLE("[DISPLAY] Initializing ST7701 via bit-banged 3-wire SPI (Required for 9-bit mode)...\n");
    
    const gpio_num_t cs_pin = (gpio_num_t)config.pins.cs;
    const gpio_num_t sck_pin = (gpio_num_t)config.pins.sck;
    const gpio_num_t sda_pin = (gpio_num_t)config.pins.sda; // MOSI/SDO
    
    // Configure pins as GPIO OUTPUT
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << sck_pin) | (1ULL << sda_pin);
    if (config.pins.cs >= 0) {
        io_conf.pin_bit_mask |= (1ULL << cs_pin);
    }
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Reset LCD (critical: vendor always drives RST high at minimum)
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
    
    // Configure backlight pin as OUTPUT, set HIGH (off) initially per vendor
    if (config.pins.bl >= 0) {
        LOG_BLE("[DISPLAY] Configuring backlight pin %d (will enable after LVGL)\n", config.pins.bl);
        gpio_set_direction((gpio_num_t)config.pins.bl, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)config.pins.bl, 1);  // HIGH = off for inverted backlight
    }
    
    // Initial pin states (Idle High)
    if (config.pins.cs >= 0) gpio_set_level(cs_pin, 1);
    gpio_set_level(sck_pin, 1);
    gpio_set_level(sda_pin, 1);
    
    // Helper lambda for 9-bit SPI write (1 D/C bit + 8 Data bits)
    auto spi_write_9bit = [&](uint16_t data) {
        for (uint8_t n = 0; n < 9; n++) {
            if (data & 0x0100) {
                gpio_set_level(sda_pin, 1);
            } else {
                gpio_set_level(sda_pin, 0);
            }
            data = data << 1;

            gpio_set_level(sck_pin, 0);
            udelay(10);
            gpio_set_level(sck_pin, 1);
            udelay(10);
        }
    };

    // Helper for writing command (D/C bit = 0)
    auto write_cmd = [&](uint8_t cmd) {
        if (config.pins.cs >= 0) {
            gpio_set_level(cs_pin, 0);
            udelay(10);
        }
        
        spi_write_9bit((uint16_t)cmd & 0x00FF); // Bit 8 is 0 (Command)
        
        udelay(10);
        if (config.pins.cs >= 0) {
            gpio_set_level(cs_pin, 1);
        }
        gpio_set_level(sck_pin, 1);
        gpio_set_level(sda_pin, 1);
        udelay(10);
    };

    // Helper for writing data (D/C bit = 1)
    auto write_data = [&](uint8_t data) {
        if (config.pins.cs >= 0) {
            gpio_set_level(cs_pin, 0);
            udelay(10);
        }
        
        spi_write_9bit(((uint16_t)data & 0x00FF) | 0x0100); // Bit 8 is 1 (Data)
        
        udelay(10);
        if (config.pins.cs >= 0) {
            gpio_set_level(cs_pin, 1);
        }
        gpio_set_level(sck_pin, 1);
        gpio_set_level(sda_pin, 1);
        udelay(10);
    };

    LOG_BLE("[DISPLAY] Sending %d ST7701 init commands...\n", 
            sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]));
    
    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]); i++) {
        write_cmd(st7701_init_cmds[i].cmd);
        
        const uint8_t* data_ptr = (const uint8_t*)st7701_init_cmds[i].data;
        for(size_t j=0; j<st7701_init_cmds[i].data_bytes; j++) {
            write_data(data_ptr[j]);
        }
        
        if (st7701_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7701_init_cmds[i].delay_ms));
        }
    }
    
    LOG_BLE("[DISPLAY] ST7701 init commands sent successfully\n");
    
    // Give the panel time to fully initialize before RGB data starts
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // CRITICAL: Reset GPIO pins to release them from SPI mode
    // The RGB panel driver will reconfigure these pins for parallel data
    gpio_reset_pin(sck_pin);
    gpio_reset_pin(sda_pin);
    
    LOG_BLE("[DISPLAY] SPI pins %d/%d reset for RGB data\n", sck_pin, sda_pin);
    
    return ESP_OK;
}

void DisplayManager::update() {
    if (!initialized) return;
    
    // Update touch driver (track touch timing for screen timeout, no LVGL calls)
    touch_driver.update();
}

void DisplayManager::draw_test_pattern() {
    if (!lcd_panel) return;

    LOG_BLE("[DISPLAY] Drawing test pattern (200x200 red square on white), res=%dx%d\n",
            screen_width, screen_height);

    // RGB panels use direct framebuffer access
    // Write to framebuffer
    void* fb0 = nullptr;
    
    esp_lcd_rgb_panel_get_frame_buffer(lcd_panel, 1, &fb0);
    
    if (!fb0) {
        LOG_BLE("[DISPLAY] ERROR: Could not get framebuffer\n");
        return;
    }
    
    const int square_size = 200;
    const uint16_t red = 0xF800;  // RGB565 red (R=31, G=0, B=0)
    const uint16_t green = 0x07E0;  // RGB565 green  
    const uint16_t blue = 0x001F;  // RGB565 blue
    const uint16_t white = 0xFFFF;  // RGB565 white
    
    // Helper to fill a framebuffer
    auto fill_fb = [&](void* fb) {
        if (!fb) return;
        uint16_t* framebuffer = (uint16_t*)fb;
        
        // Fill entire screen with green (should be very visible)
        for (int y = 0; y < screen_height; y++) {
            for (int x = 0; x < screen_width; x++) {
                framebuffer[y * screen_width + x] = green;
            }
        }
        
        // Draw red square in center
        int start_x = (screen_width - square_size) / 2;
        int start_y = (screen_height - square_size) / 2;
        
        for (int y = start_y; y < start_y + square_size && y < screen_height; y++) {
            for (int x = start_x; x < start_x + square_size && x < screen_width; x++) {
                framebuffer[y * screen_width + x] = red;
            }
        }
        
        // Draw blue square in top-left
        for (int y = 0; y < 100 && y < screen_height; y++) {
            for (int x = 0; x < 100 && x < screen_width; x++) {
                framebuffer[y * screen_width + x] = blue;
            }
        }
    };
    
    // Fill framebuffer
    fill_fb(fb0);
    
    // Flush cache to ensure DMA sees the data in PSRAM
    // if (fb0) esp_cache_msync(fb0, screen_width * screen_height * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    
    LOG_BLE("[DISPLAY] Test pattern written to framebuffer\n");
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
