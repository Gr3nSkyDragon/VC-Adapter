# VC-Adapter

Firmware, PC tools and a PCB for an ESP32-S3 board that carries Nintendo 3DS local wireless (UDS) for Azahar and mGBA-LDN, and
lets a real Game Boy cartridge trade with a 3DS's Virtual Console Pokemon games (Red, Blue, Yellow, Gold, Silver, Crystal).
Firmware 2.1-ldn and 2.1-aio also carry GB-Link Switch LDN's bridge, for GBA games with a Switch.

## Firmware versions

One codebase, three firmware lines (`firmware/esp32-uds-bridge`, `package-release-bin.ps1 -Line uds`, `-Line ldn` or `-Line aio`;
`firmware/esp32-uds-bridge/FIRMWARE.md` has the chips and what is alpha). **`uds` changed meaning in 2.1:** 2.0-uds had both sides
and is 2.1-ldn now.

| | Firmware 2.1-uds (new) | Firmware 2.1-ldn (was 2.0-uds, 1.8 `gblink`) | Firmware 2.1-aio (was 2.0 and 1.9, `vcadapter`) |
| --- | --- | --- | --- |
| Board | Any ESP32-S3 on a PC's or phone's USB, for Azahar and PokeUDS | Any ESP32-S3 on a PC's or phone's USB, for mGBA-LDN | The standalone VC-Adapter board for the GB/GBA games: a XIAO ESP32-S3 on the console's link port (`VC-Adapter-DIY`) |
| 3DS side | Raw radio for Azahar and PokeUDS; Virtual Console trades for mGBA, or a real cartridge through a GB-Link and `uds-gblink-relay` | The same | Virtual Console trades for the cartridge on the board's own link port, with no PC (or with `uds-gblink-relay --port` watching) |
| Switch side | None | GB-Link Switch LDN's bridge, for mGBA's Switch backend, GB-Link's clients or a GB-Link | The same bridge, with no GB-Link: FireRed, LeafGreen and Emerald through the GBA Wireless Adapter emulated on the link port, Ruby and Sapphire through their link cable (mGBA's RFU Cable Wrapper on the board) |
| Switching | None: it boots in 3DS mode, so connecting never restarts it or drops its USB port | By what the host speaks, both ways, no power cycle (each switch restarts the board) | The same as ldn; with no host, the side it was last left in |
| Image | `esp32-uds-bridge-2.1-uds-<chip>.bin` | `esp32-uds-bridge-2.1-ldn-<chip>.bin` | `esp32-uds-bridge-2.1-aio-esp32s3.bin` |

`<chip>` is `esp32s3`, or `esp32`, `esp32c3` and `esp32c6` (**alpha**: built, not yet run on hardware).

The board's GBA side on hardware (as firmware 2.0, before the merge with the 2.1 lines): the Switch side has traded both ways round
(the Switch leading a group and the GBA leading one) with FireRed and Emerald, and with Ruby through the cable, repeated trade room
visits and cartridge swaps included; the 3DS side has traded a Gen 1 cartridge with Virtual Console. Not yet tried: 2.1-aio itself,
other activities than trading (battles), and Gen 2.

Release images and `uds-esp32-setup.exe` (which flashes an image and stores the keys the board needs) are on the Releases page.
See `firmware/esp32-uds-bridge/README.md` for the hardware, flashing, the protocol and what has been verified.

| Folder | What |
| --- | --- |
| `firmware/esp32-uds-bridge` | ESP-IDF firmware for the board; `ldn/` is GB-Link Switch LDN's bridge (see its README) |
| `tools` | PC programs: `uds-esp32-setup.exe` (flash a release image and store the 3DS and Switch keys, no Python or ESP-IDF needed), `uds-gblink-relay.exe` (carries a GB-Link's frames to the board when both are on the PC's USB, or with `--port` shows the log of a VC-Adapter board) |
| `VC-Adapter-DIY` | KiCad 10 project for the VC-Adapter board: **proof of concept, PCB unfinished** (see its README) |

## Building

**Firmware:** ESP-IDF v6.1 at commit `fff9895c82d744c7237be8847347bdd1b07c6643` (GB-Link Switch LDN pins it), installed for
`esp32,esp32c3,esp32c6,esp32s3`. In PowerShell (`package-release-bin.sh` on Linux, macOS and WSL):

```
. <ESP-IDF v6.1>\export.ps1
cd firmware/esp32-uds-bridge
.\package-release-bin.ps1                 # every image into release/
.\package-release-bin.ps1 -Line uds       # 2.1-uds (3DS side only) for the four chips
.\package-release-bin.ps1 -Line ldn       # 2.1-ldn (3DS and Switch sides) for the four chips
.\package-release-bin.ps1 -Line aio       # 2.1-aio (ESP32-S3)
```

**PC tools:** from a Visual Studio x64 developer prompt:

```
cmake -S tools -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/tools
build\tools\uds-esp32-flash-test.exe
```

## The emulators

Azahar ([azahar_uds](https://github.com/Gr3nSkyDragon/azahar_uds)) and mGBA-LDN ([mgba_LDN](https://github.com/Gr3nSkyDragon/mgba_LDN))
each keep their own copy of the PC side of the USB framing, built into the emulator. The firmware's `main/uds_wire.h` is the
reference for it: a change to the framing changes this repository and both emulators. `firmware/esp32-uds-bridge/test/wire_test.c`
shares golden vectors with Azahar's `src/tests/core/hle/service/nwm/esp32_wire.cpp`.

## Licences

Firmware and PC tools: GPL-3.0-or-later (`LICENSE`). GB-Link Switch LDN's bridge (`firmware/esp32-uds-bridge/ldn`) is
AGPL-3.0, its LDN protocol components GPL-3.0 (their own `LICENSE` and `LDN-GPL-3.0.txt`); firmware 2.1-ldn (2.0-uds, 1.8) and
2.1-aio (2.0) contain it, so such an image as a whole is distributed under AGPL-3.0. Firmware 2.1-uds and 1.9 leave it out. 2.1-aio
also contains the Wireless Adapter emulation (`firmware/esp32-uds-bridge/rfu`, GPL-3.0, from GBLink-Firmware) and mGBA-LDN's RFU
Cable Wrapper (`firmware/esp32-uds-bridge/cable`, MPL-2.0). `firmware/esp32-uds-bridge/main/board.c` is ported from GB-Link Switch
LDN and is AGPL-3.0 too. PCB: MIT (`VC-Adapter-DIY/LICENSE`);
`VC-Adapter-DIY/gba_linkport` is MIT from GB-Link-USB-DIY (its own `LICENSE`). Three files stay MPL-2.0, as their headers say:
`tools/src/md5.c` and `md5.h` (mGBA's, by Jeffrey Pfau) and `tools/src/esp32-serial.c` (mGBA-LDN, with changes by Alex Howe).
The firmware's `main/gbwrap/include/mgba-util` stand-ins describe mGBA's interfaces.
