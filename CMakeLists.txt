cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(COBRA_HU_185)

# Create SPIFFS image from storage directory and flash it to the 'model' partition
# This will automatically include all files from the storage/ directory
# Note: First argument must match the partition name in partitions.csv
spiffs_create_partition_image(model ${CMAKE_CURRENT_SOURCE_DIR}/storage FLASH_IN_PROJECT)