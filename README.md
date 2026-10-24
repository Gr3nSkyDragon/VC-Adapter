# VC-Adapter

Firmware, PC tools and a PCB for an ESP32-S3 board that carries Nintendo 3DS local wireless (UDS) for Azahar and mGBA-LDN, and
lets a real Game Boy cartridge trade with a 3DS's Virtual Console Pokemon games (Red, Blue, Yellow, Gold, Silver, Crystal).
Both firmware builds also carry GB-Link Switch LDN's bridge, for GBA games with a Switch.

## Firmware versions

One codebase, two builds (`firmware/esp32-uds-bridge`, `package-release-bin.ps1 -Profile 1` or `-Profile 2`):

| | Firmware 1.8 (profile 1, `gblink`) | Firmware 2.0 (profile 2, `vcadapter`) |
| --- | --- | --- |
| Board | Any ESP32-S3 on a PC's or phone's USB | The VC-Adapter board: a XIAO ESP32-S3 on the console's link port (`VC-Adapter-DIY`) |
| 3DS side | Raw radio for Azahar; Virtual Console trades for mGBA, or a real cartridge through a GB-Link and `uds-gblink-relay` | Virtual Console trades for the cartridge on the board's own link port, with no PC (or with `uds-gblink-relay --port` watching) |
| Switch side | GB-Link Switch LDN's bridge, for mGBA's Switch backend, GB-Link's clients or a GB-Link | The same bridge, with no GB-Link: FireRed, LeafGreen and Emerald through the GBA Wireless Adapter emulated on the link port, Ruby and Sapphire through their link cable (mGBA's RFU Cable Wrapper on the board) |
| Switching | By what the host speaks, both ways, no power cycle | The same; with no host, the side it was last left in |
| Image | `esp32-uds-bridge-fw1.8-esp32s3-gblink.bin` | `esp32-uds-bridge-fw2.0-esp32s3.bin` |

Firmware 2.0's Switch side has traded on hardware (FireRed with the Switch, and Ruby through the cable); its 3DS side has not
been tested again since 1.9, the board's first, Game Boy only build (`esp32-uds-bridge-fw1.9-esp32s3-vcadapter.bin`).

Release images and `uds-esp32-setup.exe` (which flashes an image and stores the keys the board needs) are on the Releases page.
See `firmware/esp32-uds-bridge/README.md` for the hardware, flashing, the protocol and what has been verified.

| Folder | What |
| --- | --- |
| `firmware/esp32-uds-bridge` | ESP-IDF firmware for the board; `ldn/` is GB-Link Switch LDN's bridge (see its README) |
| `tools` | PC programs: `uds-esp32-setup.exe` (flash a release image and store the 3DS and Switch keys, no Python or ESP-IDF needed), `uds-gblink-relay.exe` (carries a GB-Link's frames to the board when both are on the PC's USB, or with `--port` shows the log of a VC-Adapter board) |
| `VC-Adapter-DIY` | KiCad 10 project for the VC-Adapter board: **proof of concept, PCB unfinished** (see its README) |

## Building

**Firmware:** ESP-IDF v6.1 at commit `fff9895c82d744c7237be8847347bdd1b07c6643` (GB-Link Switch LDN pins it). In PowerShell:

```
. <ESP-IDF v6.1>\export.ps1
cd firmware/esp32-uds-bridge
.\package-release-bin.ps1               # both images into release/
.\package-release-bin.ps1 -Profile 1    # firmware 1.8 only
.\package-release-bin.ps1 -Profile 2    # firmware 2.0 only
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
AGPL-3.0, its LDN protocol components GPL-3.0 (their own `LICENSE` and `LDN-GPL-3.0.txt`); firmware 1.8 and 2.0 contain it, so
their images as a whole are distributed under AGPL-3.0 (1.9 left it out). Firmware 2.0 also contains the Wireless Adapter emulation
(`firmware/esp32-uds-bridge/rfu`, GPL-3.0, from GBLink-Firmware) and mGBA-LDN's RFU Cable Wrapper (`firmware/esp32-uds-bridge/cable`,
MPL-2.0). PCB: MIT (`VC-Adapter-DIY/LICENSE`);
`VC-Adapter-DIY/gba_linkport` is MIT from GB-Link-USB-DIY (its own `LICENSE`). Three files stay MPL-2.0, as their headers say:
`tools/src/md5.c` and `md5.h` (mGBA's, by Jeffrey Pfau) and `tools/src/esp32-serial.c` (mGBA-LDN, with changes by Alex Howe).
The firmware's `main/gbwrap/include/mgba-util` stand-ins describe mGBA's interfaces.
