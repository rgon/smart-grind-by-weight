// Prevent multiple inclusion
#pragma once

enum class DisplayProfile {
    ESP32S3_TOUCH_AMOLED_164,  // Current 280x456 QSPI
    VIEWE_TOUCH_ST7701S_ROUND_21,    // 480x480 round UEDX48480021-MD80ET
    // Add more profiles as needed
};

enum class TouchController {
    FT3168,   // FocalTech FT3168 (current)
    CST826,   // Hynitron CST826
    NONE      // No touch controller
};

struct TouchConfig {
    TouchController controller;
    uint8_t i2c_address;
    int8_t sda_pin;
    int8_t scl_pin;
    uint8_t num_touches_reg;  // Register to read number of touches
    uint8_t touch_data_reg;   // Register to read touch data
};

enum class DisplayInterfaceType {
    QSPI,
    MIPI_PARALLEL,
    // Add more interface types as needed
};

struct DisplayPinConfig {
    // Common pins
    int8_t rst = -1;
    int8_t bl = -1;

    // SPI/QSPI pins
    int8_t cs = -1;
    int8_t sck = -1;

    // SPI data pin (also used for RGB panel for data)
    int8_t sda = -1;  // Used as data pin in RGB mode

    // QSPI data pins
    int8_t d0 = -1; // Used as SDA in SPI mode, or in RGB SPI side channel
    int8_t d1 = -1;
    int8_t d2 = -1;
    int8_t d3 = -1;
    
    // MIPI/RGB pins
    int8_t de = -1;
    int8_t vsync = -1;
    int8_t hsync = -1;
    int8_t pclk = -1;
    
    // RGB Parallel - Color data pins
    int8_t r0 = -1, r1 = -1, r2 = -1, r3 = -1, r4 = -1;
    int8_t g0 = -1, g1 = -1, g2 = -1, g3 = -1, g4 = -1, g5 = -1;
    int8_t b0 = -1, b1 = -1, b2 = -1, b3 = -1, b4 = -1;
};

struct DisplayConfig {
    // Display dimensions
    uint16_t width;
    uint16_t height;
    
    // Profile identification
    DisplayProfile profile;
    bool is_round;
    
    // Display configuration
    uint8_t rotation = 0;
    int16_t offset_x = 0;
    int16_t offset_y = 0;
    
    // Interface configuration
    DisplayInterfaceType interface;
    const char* driver_name;  // "CO5300", "ST7701", "ST7789", etc.
    
    // Display-specific settings
    uint8_t color_order;        // RGB/BGR ordering
    int16_t ips_invert_x;       // IPS X-axis inversion
    int16_t ips_invert_y;       // IPS Y-axis inversion
    uint8_t min_brightness_pct; // Minimum brightness percentage
    bool inverted_backlight = false;  // If true, brightness is inverted (1.0 = off, 0.0 = full)

    // Pin configuration
    DisplayPinConfig pins;
    
    // Touch configuration
    TouchConfig touch;
};

// Compile-time display selection
#ifndef DISPLAY_PROFILE
    #define DISPLAY_PROFILE DisplayProfile::VIEWE_TOUCH_ST7701S_ROUND_21
#endif

// Display configurations database
constexpr DisplayConfig DISPLAY_CONFIGS[] = {
    // ESP32S3_TOUCH_AMOLED_164 - Current Waveshare AMOLED 1.64"
    {
        .width = 280,
        .height = 456,
        .profile = DisplayProfile::ESP32S3_TOUCH_AMOLED_164,
        .is_round = false,
        .interface = DisplayInterfaceType::QSPI,
        .driver_name = "CO5300",
        .color_order = 20,
        .ips_invert_x = 180,
        .ips_invert_y = 24,
        .min_brightness_pct = 15,
        // RGB timing (unused for QSPI)
        .pins = {
            .rst = 21,
            .bl = -1,  // Controlled via driver
            .cs = 9,
            .sck = 10,
            .sda = -1,
            .d0 = 11,
            .d1 = 12,
            .d2 = 13,
            .d3 = 14,
            .de = -1,
            .vsync = -1,
            .hsync = -1,
            .pclk = -1,
            .r0 = -1, .r1 = -1, .r2 = -1, .r3 = -1, .r4 = -1,
            .g0 = -1, .g1 = -1, .g2 = -1, .g3 = -1, .g4 = -1, .g5 = -1,
            .b0 = -1, .b1 = -1, .b2 = -1, .b3 = -1, .b4 = -1
        },
        .touch = {
            .controller = TouchController::FT3168,
            .i2c_address = 0x38,
            .sda_pin = 47,
            .scl_pin = 48,
            .num_touches_reg = 0x02,
            .touch_data_reg = 0x03
        }
    },
    
    // VIEWE_TOUCH_ST7701S_ROUND_21 - VIEWE 2.1" Round Display
    {
        .width = 480,
        .height = 480,
        .profile = DisplayProfile::VIEWE_TOUCH_ST7701S_ROUND_21,
        .is_round = true,
        .interface = DisplayInterfaceType::MIPI_PARALLEL,  // ST7701 uses RGB parallel
        .driver_name = "ST7701",
        .color_order = 1,  // BGR for ST7701
        .ips_invert_x = 0,
        .ips_invert_y = 0,
        .min_brightness_pct = 10,
        .inverted_backlight = true,  // ST7701 backlight: 0xFF = off, 0x00 = full brightness
        .pins = {
            .rst = 8,
            .bl = 7,
            .cs = 18,
            .sck = 1,
            .sda = 2,
            .d0 = -1,
            .d1 = -1,
            .d2 = -1,
            .d3 = -1,
            .de = 17,
            .vsync = 3,
            .hsync = 46,
            .pclk = 9,
            .r0 = 40, .r1 = 41, .r2 = 42, .r3 = 2, .r4 = 1,
            .g0 = 21, .g1 = 47, .g2 = 48, .g3 = 45, .g4 = 38, .g5 = 39,
            .b0 = 10, .b1 = 11, .b2 = 12, .b3 = 13, .b4 = 14
        },
        .touch = {
            .controller = TouchController::CST826,
            .i2c_address = 0x15,
            .sda_pin = 16,
            .scl_pin = 15,
            .num_touches_reg = 0x02,  // CST8XX_REG_NUMTOUCHES
            .touch_data_reg = 0x03    // CST8XX_REG_TOUCHDATA
        }
    }
};

// Helper to get active config at compile time
constexpr const DisplayConfig& get_display_config(DisplayProfile profile) {
    for (const auto& cfg : DISPLAY_CONFIGS) {
        if (cfg.profile == profile) return cfg;
    }
    return DISPLAY_CONFIGS[0];  // fallback
}

// Active configuration
constexpr DisplayConfig ACTIVE_DISPLAY = get_display_config(DISPLAY_PROFILE);

// Save display width as constants
constexpr uint16_t HW_DISPLAY_WIDTH_PX = ACTIVE_DISPLAY.width;
constexpr uint16_t LV_DISPLAY_SIZE_CONTENT_WIDTH_PX = ACTIVE_DISPLAY.width - 20;
constexpr uint16_t HW_DISPLAY_HEIGHT_PX = ACTIVE_DISPLAY.height;
constexpr uint16_t LV_DISPLAY_SIZE_CONTENT_HEIGHT_PX = ACTIVE_DISPLAY.height - 76;