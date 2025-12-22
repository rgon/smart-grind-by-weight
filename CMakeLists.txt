cmake_minimum_required(VERSION 3.16.0)
set(EXTRA_COMPONENT_DIRS src)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart-grind-by-weight)

# Add include directory for lv_conf.h to be available globally
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/include" APPEND)