# Smart Grind by Weight - ESP-IDF Pure Implementation

# Collect all source files under src/
file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       REQUIRES bt
                       REQUIRES littlefs
                       REQUIRES lvgl
                       REQUIRES esp_lvgl_port)
