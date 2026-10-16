# ESP-IDF 6 split the old `driver` component into one component per peripheral; 5.2 still has `driver`. Both build: 5.2.8 for the
# current releases, 6.1 for the merge with GB-Link Switch LDN, which pins that version.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(driver_requires esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_uart esp_driver_i2c)
else()
    set(driver_requires driver)
endif()

idf_component_register(
    SRCS "main.c" "radio.c" "uds_wire.c" "keys.c" "gbwrap.c" "display.c" "scene.c" "screen.c"
         # The Game Boy wrapper, copied unchanged from mGBA (mgba_LDN, src/gb/sio): see gbwrap/README.md.
         "gbwrap/uds-wire.c" "gbwrap/uds-cable.c" "gbwrap/uds-session.c" "gbwrap/uds-pia.c" "gbwrap/uds-room.c"
         "gbwrap/uds-ccmp.c" "gbwrap/uds-clock.c"
    INCLUDE_DIRS "." "gbwrap/include"
    REQUIRES esp_wifi esp_event esp_netif esp_timer nvs_flash esp_ringbuf ${driver_requires}
             esp_hw_support esp_system esp_rom
)

# radio.c overrides ieee80211_raw_frame_sanity_check (defined in the Wi-Fi driver library) so that
# authentication, association and beacon frames can be injected. Allow the duplicate definition.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-zmuldefs")
