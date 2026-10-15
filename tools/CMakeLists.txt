cmake_minimum_required(VERSION 3.16)
project(vc-adapter-tools C)

# The PC programs for the board. The C runtime is linked in so that they run on a PC without the Visual C++ redistributable.
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
	# Flashes esp32-uds-bridge and stores the 3DS key on the board, with a window: one file to hand out with the firmware.
	add_executable(uds-esp32-setup WIN32
		src/uds-esp32-setup.c
		src/uds-esp32-flash.c
		src/uds-esp32.c
		src/uds-keyfile.c
		src/esp32-serial.c
		src/md5.c)
	target_link_libraries(uds-esp32-setup setupapi comctl32 comdlg32 advapi32)

	# A real cartridge behind a GB-Link and an ESP32 board, both on this PC's USB: carries the Pico's frames between them.
	add_executable(uds-gblink-relay
		src/uds-gblink-relay.c
		src/uds-esp32.c
		src/uds-keyfile.c
		src/esp32-serial.c)
	target_link_libraries(uds-gblink-relay setupapi winmm)
endif()

# The flasher's ROM loader client against a simulated ESP32-S3.
add_executable(uds-esp32-flash-test
	src/uds-esp32-flash-test.c
	src/uds-esp32-flash.c
	src/md5.c)

enable_testing()
add_test(NAME uds-esp32-flash-test COMMAND uds-esp32-flash-test)
