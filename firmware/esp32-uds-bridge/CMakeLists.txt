cmake_minimum_required(VERSION 3.16)
# One source tree for ESP32, S2, S3, C3 and C6. See FIRMWARE.md for coverage. Pick a target with `idf.py set-target <chip>` or `-D IDF_TARGET=<chip>`; with neither,
# the build is for the S3, as before. Each chip's settings are in sdkconfig.defaults.<chip>, which ESP-IDF reads after
# sdkconfig.defaults (and sdkconfig.aio.<chip> after sdkconfig.aio, if there is one). package-release-bin.ps1 / .sh build them all.
if(NOT IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET esp32s3 CACHE STRING "the chip to build for: esp32s3 (default), esp32, esp32s2, esp32c3 or esp32c6")
endif()
# GB-Link Switch LDN's vendored zstd (the Switch side's Pia decompression).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/ldn/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_uds_bridge)
