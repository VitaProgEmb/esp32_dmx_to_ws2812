cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")
set(CMAKE_C_COMPILER_LAUNCHER ccache)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)


project(dmx_sniffer)

spiffs_create_partition_image(storage www FLASH_IN_PROJECT)
