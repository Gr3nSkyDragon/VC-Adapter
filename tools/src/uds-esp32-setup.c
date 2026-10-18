/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * Licensed under the GNU General Public License, version 3 or any later version. See LICENSE at the top of this repository. */

/*
 * uds-esp32-setup: sets up an ESP32-S3 board for Game Boy Virtual Console trades with a 3DS, with no Python, ESP-IDF or mGBA. It
 * flashes a release image of the esp32-uds-bridge firmware (uds-esp32-flash.c), then stores the keys the board needs: the 3DS UDS key
 * from the user's aes_keys.txt (the same request as uds-esp32-probe --store-key), and the four Switch keys from their prod.keys that
 * GB-Link Switch LDN's side of the merged firmware reads. Only those 16-byte keys leave the PC, and they are never shown or logged.
 * Each step can be done on its own. Flashing writes the whole image, which erases the key store, so the keys are stored after the
 * flash; a board is never assumed to have them already.
 *
 * The firmware comes in two builds (package-release-bin.ps1): firmware 1.8, the GB-Link build (3DS and Switch sides), for a board
 * on the PC's USB with a GB-Link, and firmware 1.9, the VC-Adapter board build (beta, Game Boy side only), for a board wired to the
 * console's link port. The "Build for" choice picks the newest image of that build next to this program; an image's build is read
 * from its version (1.9-vcadapter), and an image from before the split counts as the GB-Link build.
 *
 * Windows only (Win32, no other dependencies). Build target: uds-esp32-setup.
 */
#include "uds-esp32-flash.h"
#include "uds-esp32.h"
#include "uds-keyfile.h"

#include "esp32-serial.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define TITLE "UDS ESP32 Setup"
#define SETTINGS_KEY "Software\\mGBA-LDN\\uds-esp32-setup"
#define MAX_PORTS 8

enum {
	ID_FW_CHECK = 100,
	ID_FW_EDIT,
	ID_FW_BROWSE,
	ID_FW_INFO,
	ID_BUILD_LABEL,
	ID_BUILD_GBLINK,
	ID_BUILD_VCADAPTER,
	ID_KEY_CHECK,
	ID_KEY_EDIT,
	ID_KEY_BROWSE,
	ID_SWITCH_CHECK,
	ID_SWITCH_EDIT,
	ID_SWITCH_BROWSE,
	ID_PORT_LABEL,
	ID_PORT_COMBO,
	ID_PORT_REFRESH,
	ID_START,
	ID_PROGRESS,
	ID_LOG,
};

enum {
	WM_APP_LOG = WM_APP + 1,
	WM_APP_PROGRESS,
	WM_APP_DONE,
};

static HWND sWindow;
static HFONT sFont;
static int sDpi = 96;
static bool sBusy;
static struct Esp32SerialPortInfo sPorts[MAX_PORTS];
static size_t sPortCount;

struct Job {
	bool flash;
	bool key;
	bool switchKeys;
	char firmware[MAX_PATH];
	char keys[MAX_PATH];
	char prodKeys[MAX_PATH];
	char port[32];
	bool usbJtag;
};

static int _px(int value) {
	return MulDiv(value, sDpi, 96);
}

enum Build {
	BUILD_UNKNOWN,
	BUILD_GBLINK,
	BUILD_VCADAPTER,
};

static const char* _buildName(enum Build build) {
	return build == BUILD_VCADAPTER ? "VC-Adapter board build" : build == BUILD_GBLINK ? "GB-Link build" : "build not marked";
}

// An image's build, from the version package-release-bin.ps1 gives it ("1.8-gblink", "1.9-vcadapter"). Unmarked: from before the
// split, or built by hand with idf.py.
static enum Build _imageBuild(const struct UDSFlashImageInfo* info) {
	if (strstr(info->version, "-vcadapter")) {
		return BUILD_VCADAPTER;
	}
	if (strstr(info->version, "-gblink")) {
		return BUILD_GBLINK;
	}
	return BUILD_UNKNOWN;
}

static HWND _item(int id) {
	return GetDlgItem(sWindow, id);
}

// Settings: the paths, so that updating a board later is one click. The keys themselves are never stored here.
static void _loadSetting(const char* name, char* out, DWORD capacity) {
	DWORD size = capacity;
	if (RegGetValueA(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS) {
		out[0] = 0;
	}
}

static void _saveSetting(const char* name, const char* value) {
	RegSetKeyValueA(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_SZ, value, (DWORD) strlen(value) + 1);
}

// Worker → window ----------------------------------------------------------------------------------------------------------------

static void _log(const char* format, ...) {
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	PostMessageA(sWindow, WM_APP_LOG, 0, (LPARAM) _strdup(text));
}

static void _flashLog(void* context, const char* text) {
	(void) context;
	_log("%s", text);
}

static void _flashProgress(void* context, size_t done, size_t total) {
	(void) context;
	PostMessageA(sWindow, WM_APP_PROGRESS, (WPARAM) (total ? done * 1000 / total : 0), 0);
}

static void _finish(bool ok, const char* summary) {
	PostMessageA(sWindow, WM_APP_DONE, ok, (LPARAM) _strdup(summary));
}

static uint8_t* _readFile(const char* path, size_t* size) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	uint8_t* data = NULL;
	if (fseek(file, 0, SEEK_END) == 0) {
		long length = ftell(file);
		if (length > 0 && length <= (64 << 20) && fseek(file, 0, SEEK_SET) == 0) {
			data = malloc((size_t) length);
			if (data && fread(data, 1, (size_t) length, file) != (size_t) length) {
				free(data);
				data = NULL;
			}
			*size = (size_t) length;
		}
	}
	fclose(file);
	return data;
}

// After a flash the board restarts, and its USB port may come back a moment later: keep trying to open it.
static bool _openBoard(struct UDSEsp32* esp, const char* port, unsigned timeoutMs) {
	static const struct UDSEsp32Handlers handlers = {0};
	ULONGLONG start = GetTickCount64();
	do {
		if (udsEsp32Open(esp, port, &handlers)) {
			return true;
		}
		Sleep(500);
	} while (GetTickCount64() - start < timeoutMs);
	return false;
}

static DWORD WINAPI _work(LPVOID param) {
	struct Job* job = param;
	uint8_t key[16];
	uint8_t switchKeys[UDS_PROD_KEY_COUNT][16];
	uint8_t* image = NULL;
	size_t size = 0;
	bool ok = false;
	char error[400] = "";
	const char* summary = "";

	// Everything that can be checked is checked before the board is touched.
	if (job->key) {
		enum UDSKeyStatus status = udsKeyFileLoad(job->keys, key);
		if (!udsKeyStatusOk(status)) {
			_log("Keys: %s", udsKeyStatusText(status));
			summary = "The key file has no usable 3DS UDS key (slot 0x2D). Nothing was changed on the board.";
			goto done;
		}
		_log("Keys: %s", udsKeyStatusText(status));
	}
	if (job->switchKeys) {
		enum UDSProdKey which = UDS_PROD_KEY_KEK;
		enum UDSProdKeysStatus status = udsProdKeysLoad(job->prodKeys, switchKeys, &which);
		char text[160];
		udsProdKeysStatusText(status, which, text, sizeof(text));
		_log("Switch keys: %s", text);
		if (status != UDS_PROD_KEYS_OK) {
			summary = "The prod.keys file does not have the four Switch keys the board needs (see the log). Nothing was changed on the "
			          "board.";
			goto done;
		}
	}
	if (job->flash) {
		image = _readFile(job->firmware, &size);
		if (!image) {
			_log("Firmware: cannot read %s", job->firmware);
			summary = "The firmware file cannot be read. Nothing was changed on the board.";
			goto done;
		}
		struct UDSFlashImageInfo info;
		if (!udsFlashCheckImage(image, size, &info, error, sizeof(error))) {
			_log("Firmware: %s", error);
			summary = "That firmware file cannot be flashed. Nothing was changed on the board.";
			goto done;
		}
		_log("Firmware: %s %s (%s), built %s %s (%u KB)", info.project[0] ? info.project : "(unnamed)", info.version,
		     _buildName(_imageBuild(&info)), info.date, info.time, (unsigned) ((size + 1023) / 1024));

		struct UDSFlashPort* port = udsFlashOpenPort(job->port);
		if (!port) {
			_log("Cannot open %s. Close mGBA, Azahar or any serial monitor that is using the board, then try again.", job->port);
			summary = "The board's port could not be opened. Nothing was changed on the board.";
			goto done;
		}
		struct UDSFlashHandlers handlers = {NULL, _flashLog, _flashProgress};
		struct UDSFlashOptions options = {job->usbJtag};
		bool flashed = udsFlashWriteImage(port, &handlers, &options, image, size, error, sizeof(error));
		udsFlashClosePort(port);
		if (!flashed) {
			_log("Flashing failed: %s", error);
			summary = "Flashing failed (see the log).";
			goto done;
		}
		_log("Waiting for the new firmware to start");
		Sleep(1500);
	}

	// The firmware: say hello, then the key.
	{
		struct UDSEsp32 esp;
		_log("Talking to the firmware on %s", job->port);
		if (!_openBoard(&esp, job->port, 10000) && !_openBoard(&esp, NULL, 3000)) {
			_log("The board's port did not come back. Unplug the board and plug it in again, then store the key on its own.");
			summary = job->flash ? "Flashed, but the board could not be reached afterwards (see the log)." : "The board's port could not be opened.";
			goto done;
		}
		bool ready = udsEsp32WaitReady(&esp, 15000);
		if (!ready) {
			// Still in download mode? Restart it into the firmware once more and ask again.
			udsEsp32Close(&esp);
			struct UDSFlashPort* port = udsFlashOpenPort(job->port);
			if (port) {
				_log("No answer yet: restarting the board into its firmware");
				struct UDSFlashOptions options = {job->usbJtag};
				udsFlashRestart(port, &options);
				udsFlashClosePort(port);
				Sleep(1500);
				ready = _openBoard(&esp, job->port, 10000) && udsEsp32WaitReady(&esp, 15000);
			}
		}
		if (!ready) {
			udsEsp32Close(&esp);
			_log("The firmware did not answer. A board running another firmware (the GB-Link Switch LDN firmware, for one) does not speak "
			     "this protocol: flash esp32-uds-bridge first.");
			summary = "The board's firmware did not answer (see the log).";
			goto done;
		}
		_log("Firmware %u.%u is running (protocol %u, %s)", esp.info.major, esp.info.minor, esp.info.proto, udsEsp32BuildText(&esp));
		if ((job->key || job->switchKeys) && !udsEsp32HasGbWrapper(&esp)) {
			udsEsp32Close(&esp);
			_log("This firmware keeps no keys: version 1.4 or later is needed");
			summary = "The board's firmware is too old to keep keys. Flash 1.4 or later.";
			goto done;
		}
		if (job->key) {
			bool stored = udsEsp32SetKey(&esp, UDS_ESP32_KEY_SLOT_DATA, key, 3000);
			int present = stored ? udsEsp32KeyStatus(&esp, 1000) : -1;
			if (!stored || present != 1) {
				udsEsp32Close(&esp);
				_log("The board did not store the 3DS key");
				summary = "The 3DS key was not stored (see the log).";
				goto done;
			}
			_log("The 3DS UDS key is stored on the board");
		}
		if (job->switchKeys) {
			size_t k;
			for (k = 0; k < UDS_PROD_KEY_COUNT; ++k) {
				const uint8_t slot = (uint8_t) (UDS_ESP32_KEY_SLOT_SWITCH_FIRST + k);
				const char* name = udsProdKeyName((enum UDSProdKey) k);
				int32_t result = udsEsp32SetKeyResult(&esp, slot, switchKeys[k], 3000);
				if (result == 0x106) { // ESP_ERR_NOT_SUPPORTED: firmware from before the Switch key slots
					udsEsp32Close(&esp);
					_log("This firmware cannot keep the Switch keys: flash a build from after the ESP-IDF 6.1 port");
					summary = "The board's firmware is too old to keep the Switch keys (see the log).";
					goto done;
				}
				int present = result == 0 ? udsEsp32KeyStatusSlot(&esp, slot, 1000) : -1;
				if (result != 0 || present != 1) {
					udsEsp32Close(&esp);
					_log("The board did not store %s (result %ld)", name, (long) result);
					summary = "The Switch keys were not all stored (see the log).";
					goto done;
				}
			}
			_log("The four Switch keys are stored on the board");
		}
		udsEsp32Close(&esp);
	}
	ok = true;
	if (job->flash && (!job->key || !job->switchKeys)) {
		summary = job->key ? "Done: the firmware is flashed and the 3DS key stored. Store the Switch keys too: flashing erased them."
		        : job->switchKeys ? "Done: the firmware is flashed and the Switch keys stored. Store the 3DS key too: flashing erased it."
		                          : "Done: the firmware is flashed. Store the keys before trading: flashing erased them.";
	} else {
		summary = job->flash ? "Done: the firmware is flashed and the keys are stored. The board is ready."
		                     : "Done: the keys are stored on the board.";
	}

done:
	memset(key, 0, sizeof(key));
	memset(switchKeys, 0, sizeof(switchKeys));
	free(image);
	free(job);
	_finish(ok, summary);
	return 0;
}

// The window ---------------------------------------------------------------------------------------------------------------------

static void _appendLog(const char* text) {
	HWND log = _item(ID_LOG);
	int length = GetWindowTextLengthA(log);
	SendMessageA(log, EM_SETSEL, length, length);
	SendMessageA(log, EM_REPLACESEL, FALSE, (LPARAM) text);
	SendMessageA(log, EM_REPLACESEL, FALSE, (LPARAM) "\r\n");
}

static void _refreshPorts(void) {
	HWND combo = _item(ID_PORT_COMBO);
	char selected[32] = "";
	int current = (int) SendMessageA(combo, CB_GETCURSEL, 0, 0);
	if (current >= 0 && (size_t) current < sPortCount) {
		snprintf(selected, sizeof(selected), "%s", sPorts[current].name);
	}
	SendMessageA(combo, CB_RESETCONTENT, 0, 0);
	sPortCount = Esp32SerialListEspressif(sPorts, MAX_PORTS);
	int pick = 0;
	size_t i;
	for (i = 0; i < sPortCount; ++i) {
		char text[96];
		snprintf(text, sizeof(text), "%s  (%s)", sPorts[i].name, sPorts[i].description[0] ? sPorts[i].description : "USB serial");
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) text);
		if (!strcmp(sPorts[i].name, selected)) {
			pick = (int) i;
		}
	}
	if (!sPortCount) {
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) "No board found: plug it in by its native USB port");
	}
	SendMessageA(combo, CB_SETCURSEL, pick, 0);
}

static void _showFirmwareInfo(void) {
	char path[MAX_PATH];
	GetWindowTextA(_item(ID_FW_EDIT), path, sizeof(path));
	char text[200] = "";
	if (path[0]) {
		size_t size = 0;
		uint8_t* image = _readFile(path, &size);
		struct UDSFlashImageInfo info;
		char why[300];
		if (!image) {
			snprintf(text, sizeof(text), "The file cannot be read");
		} else if (!udsFlashCheckImage(image, size, &info, why, sizeof(why))) {
			snprintf(text, sizeof(text), "%s", why);
		} else {
			snprintf(text, sizeof(text), "%s %s, %s, built %s %s", info.project[0] ? info.project : "firmware", info.version,
			         _buildName(_imageBuild(&info)), info.date, info.time);
		}
		free(image);
	}
	SetWindowTextA(_item(ID_FW_INFO), text);
}

static void _updateEnabled(void) {
	bool flash = IsDlgButtonChecked(sWindow, ID_FW_CHECK) == BST_CHECKED;
	bool key = IsDlgButtonChecked(sWindow, ID_KEY_CHECK) == BST_CHECKED;
	bool switchKeys = IsDlgButtonChecked(sWindow, ID_SWITCH_CHECK) == BST_CHECKED;
	EnableWindow(_item(ID_FW_CHECK), !sBusy);
	EnableWindow(_item(ID_KEY_CHECK), !sBusy);
	EnableWindow(_item(ID_SWITCH_CHECK), !sBusy);
	EnableWindow(_item(ID_FW_EDIT), !sBusy && flash);
	EnableWindow(_item(ID_FW_BROWSE), !sBusy && flash);
	EnableWindow(_item(ID_BUILD_GBLINK), !sBusy && flash);
	EnableWindow(_item(ID_BUILD_VCADAPTER), !sBusy && flash);
	EnableWindow(_item(ID_KEY_EDIT), !sBusy && key);
	EnableWindow(_item(ID_KEY_BROWSE), !sBusy && key);
	EnableWindow(_item(ID_SWITCH_EDIT), !sBusy && switchKeys);
	EnableWindow(_item(ID_SWITCH_BROWSE), !sBusy && switchKeys);
	EnableWindow(_item(ID_PORT_COMBO), !sBusy);
	EnableWindow(_item(ID_PORT_REFRESH), !sBusy);
	EnableWindow(_item(ID_START), !sBusy && (flash || key || switchKeys));
	SetWindowTextA(_item(ID_START), sBusy ? "Working..." : "Start");
}

static void _browse(int editId, const char* filter, const char* title) {
	char path[MAX_PATH];
	GetWindowTextA(_item(editId), path, sizeof(path));
	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = sWindow;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = path;
	ofn.nMaxFile = sizeof(path);
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileNameA(&ofn)) {
		SetWindowTextA(_item(editId), path);
		if (editId == ID_FW_EDIT) {
			_showFirmwareInfo();
		}
	}
}

static enum Build _selectedBuild(void) {
	return IsDlgButtonChecked(sWindow, ID_BUILD_VCADAPTER) == BST_CHECKED ? BUILD_VCADAPTER : BUILD_GBLINK;
}

// An image of the other build than "Build for" says is flashed only when the user says so. An unmarked image is not asked about.
static bool _buildMatches(const char* path) {
	size_t size = 0;
	uint8_t* image = _readFile(path, &size);
	struct UDSFlashImageInfo info;
	char why[300];
	enum Build build = image && udsFlashCheckImage(image, size, &info, why, sizeof(why)) ? _imageBuild(&info) : BUILD_UNKNOWN;
	free(image);
	enum Build selected = _selectedBuild();
	if (build == BUILD_UNKNOWN || build == selected) {
		return true;
	}
	char text[400];
	snprintf(text, sizeof(text),
	         "This image is the %s, but \"Build for\" says %s.\n\nThe GB-Link build is for a board on the PC's USB with a GB-Link; the "
	         "VC-Adapter board build is for a board wired to the console's link port.\n\nFlash it anyway?",
	         _buildName(build), selected == BUILD_VCADAPTER ? "VC-Adapter board" : "GB-Link");
	return MessageBoxA(sWindow, text, TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

static void _start(void) {
	struct Job* job = calloc(1, sizeof(*job));
	job->flash = IsDlgButtonChecked(sWindow, ID_FW_CHECK) == BST_CHECKED;
	job->key = IsDlgButtonChecked(sWindow, ID_KEY_CHECK) == BST_CHECKED;
	job->switchKeys = IsDlgButtonChecked(sWindow, ID_SWITCH_CHECK) == BST_CHECKED;
	GetWindowTextA(_item(ID_FW_EDIT), job->firmware, sizeof(job->firmware));
	GetWindowTextA(_item(ID_KEY_EDIT), job->keys, sizeof(job->keys));
	GetWindowTextA(_item(ID_SWITCH_EDIT), job->prodKeys, sizeof(job->prodKeys));
	int current = (int) SendMessageA(_item(ID_PORT_COMBO), CB_GETCURSEL, 0, 0);
	const char* problem = NULL;
	if (current < 0 || (size_t) current >= sPortCount) {
		problem = "No board is selected. Plug the board in by its native USB port and press Refresh.";
	} else if (job->flash && !job->firmware[0]) {
		problem = "Choose the firmware file (esp32-uds-bridge-fw<version>-esp32s3-<build>.bin).";
	} else if (job->key && !job->keys[0]) {
		problem = "Choose your aes_keys.txt.";
	} else if (job->switchKeys && !job->prodKeys[0]) {
		problem = "Choose your prod.keys (the Switch keys).";
	}
	if (problem) {
		free(job);
		MessageBoxA(sWindow, problem, TITLE, MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (job->flash && !_buildMatches(job->firmware)) {
		free(job);
		return;
	}
	snprintf(job->port, sizeof(job->port), "%s", sPorts[current].name);
	job->usbJtag = !strcmp(sPorts[current].description, "Espressif USB");
	if (job->flash) {
		_saveSetting("firmware", job->firmware);
	}
	if (job->key) {
		_saveSetting("keys", job->keys);
	}
	if (job->switchKeys) {
		_saveSetting("prodkeys", job->prodKeys);
	}
	SetWindowTextA(_item(ID_LOG), "");
	SendMessageA(_item(ID_PROGRESS), PBM_SETPOS, 0, 0);
	sBusy = true;
	_updateEnabled();
	HANDLE thread = CreateThread(NULL, 0, _work, job, 0, NULL);
	if (thread) {
		CloseHandle(thread);
	} else {
		free(job);
		sBusy = false;
		_updateEnabled();
	}
}

static HWND _control(const char* cls, const char* text, DWORD style, int id, int x, int y, int w, int h) {
	HWND hwnd = CreateWindowExA(!strcmp(cls, "EDIT") && !(style & ES_READONLY) ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
	                            _px(x), _px(y), _px(w), _px(h), sWindow, (HMENU) (INT_PTR) id, GetModuleHandleA(NULL), NULL);
	SendMessageA(hwnd, WM_SETFONT, (WPARAM) sFont, TRUE);
	return hwnd;
}

// The newest esp32-uds-bridge release image of this build next to this program, as a release folder holds them. Images from before
// the split (esp32-uds-bridge-fw1.7-esp32s3.bin) are GB-Link builds.
static void _defaultFirmware(enum Build build, char* out, size_t capacity) {
	static const char* const kGbLink[] = {"esp32-uds-bridge-fw*-esp32s3-gblink.bin", "esp32-uds-bridge-fw*-esp32s3.bin"};
	static const char* const kVcAdapter[] = {"esp32-uds-bridge-fw*-esp32s3-vcadapter.bin"};
	const char* const* patterns = build == BUILD_VCADAPTER ? kVcAdapter : kGbLink;
	size_t count = build == BUILD_VCADAPTER ? sizeof(kVcAdapter) / sizeof(kVcAdapter[0]) : sizeof(kGbLink) / sizeof(kGbLink[0]);
	char dir[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, dir, sizeof(dir));
	char* slash = length ? strrchr(dir, '\\') : NULL;
	out[0] = 0;
	if (!slash) {
		return;
	}
	slash[1] = 0;
	FILETIME newest = {0};
	size_t i;
	for (i = 0; i < count; ++i) {
		char pattern[MAX_PATH + 48];
		snprintf(pattern, sizeof(pattern), "%s%s", dir, patterns[i]);
		WIN32_FIND_DATAA found;
		HANDLE find = FindFirstFileA(pattern, &found);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if (CompareFileTime(&found.ftLastWriteTime, &newest) > 0) {
				newest = found.ftLastWriteTime;
				snprintf(out, capacity, "%s%s", dir, found.cFileName);
			}
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
}

// "Build for" changed (or the window opened): the newest image of that build next to this program, if there is one.
static void _pickFirmware(void) {
	char path[MAX_PATH];
	_defaultFirmware(_selectedBuild(), path, sizeof(path));
	if (path[0]) {
		SetWindowTextA(_item(ID_FW_EDIT), path);
	}
	_showFirmwareInfo();
}

// Where Azahar (or Citra) keeps aes_keys.txt, if it is there.
static void _defaultKeys(char* out, size_t capacity) {
	static const char* const kApps[] = {"Azahar", "Citra"};
	char appData[MAX_PATH];
	out[0] = 0;
	if (!GetEnvironmentVariableA("APPDATA", appData, sizeof(appData))) {
		return;
	}
	size_t i;
	for (i = 0; i < sizeof(kApps) / sizeof(kApps[0]); ++i) {
		snprintf(out, capacity, "%s\\%s\\sysdata\\aes_keys.txt", appData, kApps[i]);
		if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
	}
	out[0] = 0;
}

// A prod.keys next to this program, or where the common Switch emulators keep it, if it is there.
static void _defaultProdKeys(char* out, size_t capacity) {
	static const char* const kPlaces[] = {"Ryujinx\\system", "yuzu\\keys", "suyu\\keys", "sudachi\\keys", "citron\\keys", "eden\\keys"};
	char dir[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, dir, sizeof(dir));
	char* slash = length ? strrchr(dir, '\\') : NULL;
	if (slash) {
		slash[1] = 0;
		snprintf(out, capacity, "%sprod.keys", dir);
		if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
	}
	char appData[MAX_PATH];
	out[0] = 0;
	if (!GetEnvironmentVariableA("APPDATA", appData, sizeof(appData))) {
		return;
	}
	size_t i;
	for (i = 0; i < sizeof(kPlaces) / sizeof(kPlaces[0]); ++i) {
		snprintf(out, capacity, "%s\\%s\\prod.keys", appData, kPlaces[i]);
		if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
	}
	out[0] = 0;
}

static void _create(void) {
	_control("BUTTON", "Flash this firmware:", BS_AUTOCHECKBOX | WS_TABSTOP, ID_FW_CHECK, 12, 12, 400, 20);
	_control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_FW_EDIT, 30, 34, 450, 24);
	_control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_FW_BROWSE, 488, 33, 100, 26);
	_control("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, ID_FW_INFO, 32, 62, 556, 18);
	_control("STATIC", "Build for:", SS_LEFT, ID_BUILD_LABEL, 32, 88, 70, 20);
	_control("BUTTON", "GB-Link + Switch (firmware 1.8)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, ID_BUILD_GBLINK, 104, 86, 230, 22);
	_control("BUTTON", "VC-Adapter board, beta (1.9)", BS_AUTORADIOBUTTON, ID_BUILD_VCADAPTER, 340, 86, 240, 22);
	_control("BUTTON", "Store the 3DS key from:", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, ID_KEY_CHECK, 12, 120, 400, 20);
	_control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_KEY_EDIT, 30, 142, 450, 24);
	_control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_KEY_BROWSE, 488, 141, 100, 26);
	_control("BUTTON", "Store the Switch keys from (prod.keys):", BS_AUTOCHECKBOX | WS_TABSTOP, ID_SWITCH_CHECK, 12, 172, 400, 20);
	_control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_SWITCH_EDIT, 30, 194, 450, 24);
	_control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_SWITCH_BROWSE, 488, 193, 100, 26);
	_control("STATIC", "Board:", SS_LEFT, ID_PORT_LABEL, 12, 234, 60, 20);
	_control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PORT_COMBO, 72, 230, 408, 200);
	_control("BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP, ID_PORT_REFRESH, 488, 229, 100, 26);
	_control("BUTTON", "Start", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_START, 12, 270, 140, 30);
	_control(PROGRESS_CLASSA, "", 0, ID_PROGRESS, 164, 276, 424, 18);
	SendMessageA(_item(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
	_control("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, ID_LOG, 12, 310, 576, 190);

	char path[MAX_PATH];
	_loadSetting("build", path, sizeof(path));
	CheckRadioButton(sWindow, ID_BUILD_GBLINK, ID_BUILD_VCADAPTER, !strcmp(path, "vcadapter") ? ID_BUILD_VCADAPTER : ID_BUILD_GBLINK);
	_defaultFirmware(_selectedBuild(), path, sizeof(path));
	if (!path[0]) {
		_loadSetting("firmware", path, sizeof(path));
	}
	SetWindowTextA(_item(ID_FW_EDIT), path);
	_loadSetting("keys", path, sizeof(path));
	if (!path[0]) {
		_defaultKeys(path, sizeof(path));
	}
	SetWindowTextA(_item(ID_KEY_EDIT), path);
	_loadSetting("prodkeys", path, sizeof(path));
	if (!path[0]) {
		_defaultProdKeys(path, sizeof(path));
	}
	SetWindowTextA(_item(ID_SWITCH_EDIT), path);
	CheckDlgButton(sWindow, ID_FW_CHECK, BST_CHECKED);
	CheckDlgButton(sWindow, ID_KEY_CHECK, BST_CHECKED);
	CheckDlgButton(sWindow, ID_SWITCH_CHECK, BST_CHECKED);
	_showFirmwareInfo();
	_refreshPorts();
	_updateEnabled();
}

static LRESULT CALLBACK _windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		sWindow = hwnd;
		_create();
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case ID_FW_CHECK:
		case ID_KEY_CHECK:
		case ID_SWITCH_CHECK:
			_updateEnabled();
			break;
		case ID_BUILD_GBLINK:
		case ID_BUILD_VCADAPTER:
			if (HIWORD(wParam) == BN_CLICKED) {
				_saveSetting("build", _selectedBuild() == BUILD_VCADAPTER ? "vcadapter" : "gblink");
				_pickFirmware();
			}
			break;
		case ID_FW_BROWSE:
			_browse(ID_FW_EDIT, "Firmware image (*.bin)\0*.bin\0All files\0*.*\0", "Choose the esp32-uds-bridge firmware");
			break;
		case ID_KEY_BROWSE:
			_browse(ID_KEY_EDIT, "Key files (*.txt)\0*.txt\0All files\0*.*\0", "Choose your aes_keys.txt");
			break;
		case ID_SWITCH_BROWSE:
			_browse(ID_SWITCH_EDIT, "Switch keys (*.keys)\0*.keys\0All files\0*.*\0", "Choose your prod.keys");
			break;
		case ID_FW_EDIT:
			if (HIWORD(wParam) == EN_KILLFOCUS) {
				_showFirmwareInfo();
			}
			break;
		case ID_PORT_REFRESH:
			_refreshPorts();
			break;
		case ID_START:
			_start();
			break;
		}
		return 0;
	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVNODES_CHANGED && !sBusy) {
			_refreshPorts();
		}
		return TRUE;
	case WM_APP_LOG:
		_appendLog((const char*) lParam);
		free((void*) lParam);
		return 0;
	case WM_APP_PROGRESS:
		SendMessageA(_item(ID_PROGRESS), PBM_SETPOS, wParam, 0);
		return 0;
	case WM_APP_DONE:
		sBusy = false;
		_updateEnabled();
		_appendLog((const char*) lParam);
		MessageBoxA(hwnd, (const char*) lParam, TITLE, MB_OK | (wParam ? MB_ICONINFORMATION : MB_ICONWARNING));
		free((void*) lParam);
		_refreshPorts();
		return 0;
	case WM_CLOSE:
		if (sBusy && MessageBoxA(hwnd, "The board is being set up. Closing now can leave it without working firmware. Close anyway?", TITLE,
		                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
			return 0;
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show) {
	(void) previous;
	(void) commandLine;
	SetProcessDPIAware();
	HDC screen = GetDC(NULL);
	sDpi = GetDeviceCaps(screen, LOGPIXELSX);
	ReleaseDC(NULL, screen);
	INITCOMMONCONTROLSEX common = {sizeof(common), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&common);
	NONCLIENTMETRICSA metrics;
	memset(&metrics, 0, sizeof(metrics));
	metrics.cbSize = sizeof(metrics);
	SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
	sFont = CreateFontIndirectA(&metrics.lfMessageFont);

	WNDCLASSA wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = _windowProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	wc.lpszClassName = "UdsEsp32Setup";
	RegisterClassA(&wc);

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	RECT rect = {0, 0, _px(600), _px(512)};
	AdjustWindowRect(&rect, style, FALSE);
	HWND hwnd = CreateWindowExA(0, wc.lpszClassName, TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
	                            NULL, NULL, instance, NULL);
	if (!hwnd) {
		return 1;
	}
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);
	MSG msg;
	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		if (!IsDialogMessageA(hwnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	DeleteObject(sFont);
	return 0;
}
