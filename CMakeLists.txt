cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "2.0.1")
project(COBRA_HU_185)

# SPIFFS from `storage/` → separate partition `userdata` (avoid overlap with ESP-SR
# `model` partition, which flashes `srmodels.bin` at the same offset otherwise).
spiffs_create_partition_image(userdata ${CMAKE_CURRENT_SOURCE_DIR}/storage FLASH_IN_PROJECT)