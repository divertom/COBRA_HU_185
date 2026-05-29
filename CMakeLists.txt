cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "2.0.1")
project(COBRA_HU_185)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Generate LVGL 8 *.bin SPIFFS payloads from raster sources before mkspiffs runs.
add_custom_target(
    convert_artwork ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/convert_artwork.py
            --src ${CMAKE_CURRENT_SOURCE_DIR}/artwork
            --dst ${CMAKE_CURRENT_SOURCE_DIR}/storage
    COMMENT "Converting artwork → storage LVGL binaries"
    VERBATIM)

# SPIFFS from `storage/` → separate partition `userdata` (avoid overlap with ESP-SR
# `model` partition, which flashes `srmodels.bin` at the same offset otherwise).
spiffs_create_partition_image(userdata ${CMAKE_CURRENT_SOURCE_DIR}/storage FLASH_IN_PROJECT
                             DEPENDS convert_artwork)