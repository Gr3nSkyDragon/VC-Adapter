# GB-Link Switch LDN's bridge (../../ldn, vendored unchanged but for the marked merge changes) as a component of its own, so that its
# static data can be placed in one block (linker.lf) and handed to the heap when the board runs its 3DS side (main/bridge_mode.c).
# It needs ESP-IDF 6.1; with 5.x the component holds only the host link (ldn/transport_*.c, which the 3DS side uses too) and the
# firmware has the 3DS side only.
include("${CMAKE_CURRENT_LIST_DIR}/../../ldn/SOURCES.cmake")

# The host link, by chip: the native USB Serial/JTAG port, or on the classic ESP32 (which has none) the console UART behind the
# board's USB bridge, as upstream's firmware/ESP32 does.
if(IDF_TARGET STREQUAL "esp32")
    set(bridge_transport ${BRIDGE_TRANSPORT_UART})
    set(bridge_transport_requires esp_driver_uart)
else()
    set(bridge_transport ${BRIDGE_TRANSPORT_USB})
    set(bridge_transport_requires esp_driver_usb_serial_jtag)
endif()

# Upstream's Wi-Fi driver hashes for ESP-IDF v6.1 (gb-link-switch-ldn c972db4, firmware/<chip>/main/CMakeLists.txt). The LDN join
# drives a private Wi-Fi interface, so the chip's Wi-Fi driver binary is pinned: a different one has to be checked on hardware again.
set(wifi_driver_sha256_esp32 "018dd68296790dadf4b4e9632f174d6c3a685ef786ec9d7429d3c4c6439f0cf1")
set(wifi_driver_sha256_esp32c3 "0fcbed322d3254063b2c191bc8b8004e703ffd640296164a6552676180909a0c")
set(wifi_driver_sha256_esp32c6 "6cd630f338006fdb7dde1f895eeceff653b1c97fdc8e6b7daeac20759690edbf")
set(wifi_driver_sha256_esp32s3 "265e5c89ac0d2a9444b5b466afee49f631ec549f96774f91a721ab5069c58e20")

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_component_register(
        SRCS ${BRIDGE_SOURCES} ${bridge_transport}
        INCLUDE_DIRS "${BRIDGE_COMMON}"
        REQUIRES ${BRIDGE_REQUIRES} ${bridge_transport_requires}
        LDFRAGMENTS "linker.lf"
    )
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        # As upstream (ldn/SOURCES.cmake, their project CMakeLists): the SDK revision and the chip's Wi-Fi driver binary are pinned.
        find_package(Git REQUIRED)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" rev-parse HEAD
            OUTPUT_VARIABLE sdk_revision OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE sdk_result)
        if(NOT sdk_result EQUAL 0 OR NOT sdk_revision STREQUAL "fff9895c82d744c7237be8847347bdd1b07c6643")
            message(FATAL_ERROR "The Switch side (ldn/) requires ESP-IDF v6.1 at fff9895c82d744c7237be8847347bdd1b07c6643")
        endif()
        if(NOT DEFINED wifi_driver_sha256_${IDF_TARGET})
            message(FATAL_ERROR "No pinned Wi-Fi driver for ${IDF_TARGET}: this firmware builds for esp32, esp32c3, esp32c6 and esp32s3")
        endif()
        bridge_pin_wifi_driver(${IDF_TARGET} "${wifi_driver_sha256_${IDF_TARGET}}")
    endif()
else()
    # Not built here since the multi-chip change (no ESP-IDF 5.x toolchain was at hand): the 3DS side alone, as before.
    idf_component_register(
        SRCS ${bridge_transport}
        INCLUDE_DIRS "${BRIDGE_COMMON}"
        REQUIRES driver
    )
endif()
