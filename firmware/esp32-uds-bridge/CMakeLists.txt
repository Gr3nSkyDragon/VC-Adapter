cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3 CACHE STRING "this project's only target")
# GB-Link Switch LDN's vendored zstd (the Switch side's Pia decompression).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/ldn/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_uds_bridge)
