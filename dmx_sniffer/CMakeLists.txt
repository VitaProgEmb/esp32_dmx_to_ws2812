cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dmx_sniffer)

# spiffs_create_partition_image(storage data FLASH_IN_PROJECT)
