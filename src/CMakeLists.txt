# Smart Grind by Weight - ESP-IDF Pure Implementation

# Collect all source files under src/
file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES littlefs
                       REQUIRES lvgl
                       REQUIRES esp_lvgl_port
                       REQUIRES nvs_flash
                       REQUIRES esp_timer
                       REQUIRES driver
                       REQUIRES esp_driver_rmt
                       REQUIRES app_update
                       REQUIRES delta
                       REQUIRES detools
                       REQUIRES esp_psram)

# Add compile definitions
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LV_LVGL_H_INCLUDE_SIMPLE
    NO_GLOBAL_UPDATE   # Required by esp32-flashz
    FZ_NOHTTPCLIENT    # Disable HTTP client (BLE only)
)
