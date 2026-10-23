/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * Licensed under the GNU General Public License, version 3 or any later version. See LICENSE at the top of this repository. */

/*
 * ldn-console: sends GB-Link Switch LDN console commands to a board in its Switch mode and prints what comes back, for reading the
 * board's state where it is (after a failed trade, say). It opens the port the way the other tools do (esp32-serial.c: DTR on, RTS
 * off), which leaves the board running; a serial monitor that toggles RTS on open resets it, and the state with it.
 *
 *   ldn-console [COMx] [command ...]
 *
 * With no port, the first Espressif board found. With no commands, the adapter's state: LDN_PICO_DATA, LDN_PICO_STATS,
 * LDN_PICO_STATUS and LDN_SHIM_LOG. Only text commands go out: no 3DS frame, so the board stays in its Switch mode.
 */
#include "esp32-serial.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static void _drain(struct Esp32Serial* port, DWORD forMs) {
	ULONGLONG until = GetTickCount64() + forMs;
	while (GetTickCount64() < until) {
		uint8_t buffer[512];
		int n = Esp32SerialRead(port, buffer, sizeof(buffer));
		if (n < 0) {
			fprintf(stderr, "The port failed.\n");
			return;
		}
		if (n > 0) {
			fwrite(buffer, 1, (size_t) n, stdout);
			fflush(stdout);
			until = GetTickCount64() + forMs; // keep reading while it talks
		} else {
			Sleep(10);
		}
	}
}

int main(int argc, char** argv) {
	static const char* const kDefault[] = {"LDN_PICO_DATA", "LDN_PICO_STATS", "LDN_PICO_STATUS", "LDN_SHIM_LOG"};
	char name[32] = "";
	int first = 1;
	if (argc > 1 && !_strnicmp(argv[1], "COM", 3)) {
		snprintf(name, sizeof(name), "%s", argv[1]);
		first = 2;
	} else if (!Esp32SerialFindEspressif(name, sizeof(name))) {
		fprintf(stderr, "No board found. Give its port: ldn-console COMx\n");
		return 1;
	}
	struct Esp32Serial* port = Esp32SerialOpen(name, 115200);
	if (!port) {
		fprintf(stderr, "Cannot open %s. Close mGBA, Azahar, the setup tool or any serial monitor using it.\n", name);
		return 1;
	}
	fprintf(stderr, "%s open\n", name);
	_drain(port, 300); // whatever the board had queued for a host

	const char* const* commands = argc > first ? (const char* const*) (argv + first) : kDefault;
	int count = argc > first ? argc - first : (int) (sizeof(kDefault) / sizeof(kDefault[0]));
	for (int i = 0; i < count; ++i) {
		char line[128];
		int length = snprintf(line, sizeof(line), "\n%s\n", commands[i]);
		if (length <= 0 || length >= (int) sizeof(line) || !Esp32SerialWrite(port, line, (size_t) length)) {
			fprintf(stderr, "Could not send %s\n", commands[i]);
			break;
		}
		_drain(port, 600);
	}
	Esp32SerialClose(port);
	return 0;
}
