# GB-Link Switch LDN's bridge (../../ldn, vendored unchanged but for the marked merge changes) as a component of its own, so that its
# static data can be placed in one block (linker.lf) and handed to the heap when the board runs its 3DS side (main/bridge_mode.c).
# It needs ESP-IDF 6.1; with 5.x the component holds only the host link (ldn/transport_*.c, which the 3DS side uses too) and the
# firmware has the 3DS side only.
include("${CMAKE_CURRENT_LIST_DIR}/../../ldn/SOURCES.cmake")

# The S2 runs UDS directly; its USB OTG port cannot use the Serial/JTAG driver.
if(IDF_TARGET STREQUAL "esp32s2")
    idf_component_register(
        SRCS "../../ldn/transport_usb_cdc.c"
        INCLUDE_DIRS "${BRIDGE_COMMON}"
        REQUIRES freertos
    )
    # No Switch side here, but the 3DS side's raw-frame override (main/radio.c, -zmuldefs) replaces a function inside the closed
    # Wi-Fi driver, so the S2's driver binary is pinned as the other chips' are: ESP-IDF v6.1's libnet80211.a for the S2, in which
    # ieee80211_raw_frame_sanity_check is a global function, as on the other chips. A different binary has to be checked again.
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        bridge_pin_wifi_driver(esp32s2 "df1a279db19ac84febf0c3b140f9c2a20d2ca2b629463bbc80641fa0d5d74511")
    endif()
    return()
endif()

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
    # The VC-Adapter board has no GB-Link: its GBA end is the wireless adapter emulated on its own link port pins (../../rfu,
    # CONFIG_UDS_RFU_ADAPTER), which pico_link uses in place of its UART. In this component too, so that its static data is
    # handed to the heap in 3DS mode with the rest.
    set(rfu_sources)
    if(CONFIG_UDS_RFU_ADAPTER)
        set(rfu_sources "${CMAKE_CURRENT_LIST_DIR}/../../rfu/rfu_adapter.cpp")
    endif()
    # Ruby and Sapphire's cable (../../cable, CONFIG_UDS_RFU_CABLE): mGBA's RFU Cable Wrapper, its wireless side unchanged and
    # built against the stand-in mGBA headers in cable/compat.
    set(cable_sources)
    set(cable_private_includes)
    if(CONFIG_UDS_RFU_CABLE)
        set(cable_sources "${CMAKE_CURRENT_LIST_DIR}/../../cable/rfu-wrapper-air.c" "${CMAKE_CURRENT_LIST_DIR}/../../cable/cable_air.c"
            "${CMAKE_CURRENT_LIST_DIR}/../../cable/cable_master.c")
        set(cable_private_includes "${CMAKE_CURRENT_LIST_DIR}/../../cable/compat")
    endif()
    idf_component_register(
        SRCS ${BRIDGE_SOURCES} ${bridge_transport} ${rfu_sources} ${cable_sources}
        INCLUDE_DIRS "${BRIDGE_COMMON}" "${CMAKE_CURRENT_LIST_DIR}/../../rfu" "${CMAKE_CURRENT_LIST_DIR}/../../cable"
        PRIV_INCLUDE_DIRS ${cable_private_includes}
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
