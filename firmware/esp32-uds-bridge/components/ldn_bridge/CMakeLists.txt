# GB-Link Switch LDN's bridge (../../ldn, vendored unchanged but for the marked merge changes) as a component of its own, so that its
# static data can be placed in one block (linker.lf) and handed to the heap when the board runs its 3DS side (main/bridge_mode.c).
# It needs ESP-IDF 6.1; with 5.x the component is empty and the firmware has the 3DS side only.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    include("${CMAKE_CURRENT_LIST_DIR}/../../ldn/SOURCES.cmake")
    idf_component_register(
        SRCS ${BRIDGE_SOURCES} ${BRIDGE_TRANSPORT_USB}
        INCLUDE_DIRS "${BRIDGE_COMMON}"
        REQUIRES ${BRIDGE_REQUIRES} esp_driver_usb_serial_jtag
        LDFRAGMENTS "linker.lf"
    )
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        # As upstream (ldn/SOURCES.cmake, their project CMakeLists): the LDN join drives a private Wi-Fi interface, so the SDK
        # revision and the chip's Wi-Fi driver binary are pinned.
        find_package(Git REQUIRED)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" rev-parse HEAD
            OUTPUT_VARIABLE sdk_revision OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE sdk_result)
        if(NOT sdk_result EQUAL 0 OR NOT sdk_revision STREQUAL "fff9895c82d744c7237be8847347bdd1b07c6643")
            message(FATAL_ERROR "The Switch side (ldn/) requires ESP-IDF v6.1 at fff9895c82d744c7237be8847347bdd1b07c6643")
        endif()
        bridge_pin_wifi_driver(esp32s3 "265e5c89ac0d2a9444b5b466afee49f631ec549f96774f91a721ab5069c58e20")
    endif()
else()
    idf_component_register()
endif()
