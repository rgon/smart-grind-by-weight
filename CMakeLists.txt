cmake_minimum_required(VERSION 3.16.0)

# Global compile options
add_compile_options(-fdiagnostics-color=always -DLV_LVGL_H_INCLUDE_SIMPLE -Wno-format -w)

set(EXTRA_COMPONENT_DIRS src)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Make lv_conf.h discoverable for all components (including LVGL)
# LV_CONF_PATH must be a C string literal
idf_build_set_property(COMPILE_DEFINITIONS "LV_CONF_PATH=\"${CMAKE_SOURCE_DIR}/include/lv_conf.h\"" APPEND)

project(smart-grind-by-weight)