# esp32-uds-bridge

Firmware for an **ESP32-S3** that acts as the 802.11 radio for Azahar's local wireless (UDS) on
Android, in place of the USB Wi-Fi adapter and `ldnd.exe` used on Windows. The phone talks to the
board over USB-C; the board captures and injects raw Wi-Fi frames so the emulator can trade with a
retail 3DS.

It is deliberately dumb. It has no LDN/PIA logic (unlike GB-Link-Switch-LDN): CCMP encryption, the
authentication/association handshake, beacons' contents and everything else stay in Azahar's
`nwm::UDS`. The board only does what a phone cannot:

| Job | How |
| --- | --- |
| Receiving unicast frames sent to the emulated 3DS | The hardware is started with a *twin* of the emulated MAC (first octet XOR 0x02), not the MAC itself. The chip does not hand unicast data frames addressed to its own address to the capture path, which broke the join handshake; with a twin address those frames are ordinary sniffed traffic. The consequence is that the hardware does not ACK them and the peer retransmits a few frames. The peer does ACK every unicast frame the board sends (`txUnicast == acksRx` in the stats). |
| Capture | Promiscuous mode (management + data). Only frames that matter are forwarded: beacons carrying a Nintendo (00:1F:32) vendor element, and any frame touching the emulated MAC or the "watched" peer MAC. |
| Inject | `esp_wifi_80211_tx` with the sequence control field left alone. `ieee80211_raw_frame_sanity_check` is overridden (see `main/CMakeLists.txt`, `-zmuldefs`) so authentication/association/beacon frames are allowed. |
| Beacon cadence | Azahar hands over a beacon template; the board sends it every 102.4 ms with a fresh timestamp and sequence number, so USB jitter never shows up as a missing beacon. |
| Channel | Azahar drives discovery hopping (1, 6, 11) and channel selection with `SET_CHANNEL`, exactly like the desktop monitor. |

## Hardware

Any ESP32-S3 board. **Use the chip's native USB port** (the one wired to GPIO19/20, usually labelled
"USB" rather than "UART"/"COM"); it enumerates as Espressif USB Serial/JTAG (`303A:1001`), which is
what the app looks for. Connect it to the phone with a USB-C cable (a USB-C to USB-C cable, or an OTG
adapter). The board is powered from the phone; 2.4 GHz radio TX draws a few hundred mA.

### Optional screen (firmware 1.5)

A 128x64 SSD1306 I2C OLED (the common four-pin 0.96" module): **SDA to GPIO8, SCL to GPIO9, VCC to
3V3, GND to GND**. The firmware looks for it at 0x3C, then 0x3D, once at boot, so connect it before
powering the board; without one the board runs exactly as before. It shows the firmware and whether a
host has said HELLO, the raw radio's channel and frame counts while Azahar drives it, and the Game Boy
wrapper's game, join/session/cable stage and frame counts. After a minute with nothing happening it
dims, after ten it turns off; BOOT wakes it.

What it draws is `main/scene.c` (layout, icons, wording) using the calls in `main/screen.h`, on a
128x64 grid with (0, 0) at the top left. Two tools in `tools/` (Python 3, nothing to install):

- **Preview without flashing:** `python tools/screen_preview.py` builds `scene.c` with a PC compiler
  (MSYS2's gcc is found by itself) and writes every clip listed in `tools/screen_preview/preview.c`
  to `tools/screen_preview/out/`: a PNG per clip, an animated GIF for moving ones, and `all.png`
  with all of them. `--color twotone` shows the yellow-band modules; add a clip to `kClips` to
  preview a new state.
- **Pictures to C:** `python tools/img2c.py -o main/images.h art/ball.png` turns PNGs into one-bit
  images, drawn with `fb_image(fb, x, y, IMG_BALL_W, IMG_BALL_H, kImgBall, true)`. `--mode dark`
  for art drawn black on white, `--frames N` for N animation frames side by side, `--mode sprite
  --size 48` for colour Pokemon sprites; it prints each picture in the terminal, and `--preview
  out.png` writes it enlarged.

### GBA link port, no GB-Link (firmware 1.8)

The **VC-Adapter board build** (`esp32-uds-bridge-fw<version>-esp32s3-vcadapter.bin`; `sdkconfig.vcadapter`, which turns on
`CONFIG_UDS_LINK_PORT`, menuconfig's UDS bridge > **Game Boy link port wired to GPIOs**) needs no GB-Link: a GBA (playing a GB/GBC cartridge), GBC or Game Boy link port is wired to four GPIOs through a level
shifter, and the board clocks the cartridge itself, as the GB-Link's Pico did (8192 Hz, the board the master, the cartridge the
slave; `main/linkport.c`). **Not yet run on hardware.**

The port's signals are **5 V when a GBA runs a GB/GBC cartridge** (3.3 V for a GBA game). The ESP32's pins take 3.3 V only, so
a level shifter is required: a 4-channel BSS138 module (the common "I2C-safe bidirectional logic level converter"). Power its
high side from the port's own VCC (pin 1), so that it follows the console, and its low side from the board's 3V3. Never connect
pin 1 to the board's 5V or 3V3 pin, and never power the board from the port.

The defaults are the pinout shared with the GB-Link side of the unified board. Its signal names follow the GB-Link's notation:
**SI and SO are the adapter's**, not the console's, so the adapter's SI carries the console's SO and the adapter's SO carries the
console's SI.

| Adapter signal | Link port pin (console's name) | Level shifter | XIAO ESP32-S3 | GPIO (Kconfig default) |
| --- | --- | --- | --- | --- |
| SC (clock) | 5 (SC) | one channel | D0 | GPIO1 (`UDS_LINK_PORT_SC_GPIO`) |
| SI (adapter in) | 2 (SO, the console's output) | one channel | D1 | GPIO2 (`UDS_LINK_PORT_SI_GPIO`) |
| SO (adapter out) | 3 (SI, the console's input) | one channel | D2 | GPIO3 (`UDS_LINK_PORT_SO_GPIO`) |
| SD | 4 (SD) | one channel | D3 | GPIO4 (`UDS_LINK_PORT_SD_GPIO`; unused in a Game Boy's link, held as an input) |
| — | 1 (VCC, 5 V in GB mode) | HV supply | — | (optional: 100k/100k divider to D4 = GPIO5, `..._SENSE_GPIO`) |
| 3V3 | — | LV supply | 3V3 | — |
| GND | 6 (GND) | GND (both sides) | GND | — |

The port pin numbers are for a **plug that goes straight into the console** (its pin 2 is the console's SO). A **socket** on the
board with a link cable to the console is the other way round, because the cable crosses SO and SI: socket pin 3 goes to the
adapter's SI (D1) and pin 2 to its SO (D2), or swap the two GPIOs in menuconfig. Use a cable that carries GB/GBC games (a
GBC-type or universal cable).

**The sense divider (optional).** Two equal resistors (100 kΩ each) in series from the port's VCC (pin 1, the same net that
powers the shifter's HV side) to GND, with the point between them wired to an ADC pin (D4 = GPIO5 is free with the shared pinout;
set `CONFIG_UDS_LINK_PORT_SENSE_GPIO=5`). The divider halves the port's voltage into the ESP32's range (5 V reads 2.5 V), so
the board can tell nothing / a GBA game (3.3 V) / a Game Boy game (5 V) apart: it starts the wrapper on its own only for a Game
Boy game and stops 3 s after the console goes off. A 100 nF capacitor from the middle point to GND steadies the reading. Without
the divider (`-1`, the default) the board assumes a Game Boy game is there.

The defaults share GPIO1 and GPIO2 with the GB-Link UART mode's (`CONFIG_UDS_PICO_LINK`, taken from GB-Link's own ESP32 firmware);
a build with both turned on stops with an error until one of them is moved.

The pins are open drain (the shifter's pull-ups make the high level), so a cartridge clocking a byte of its own never fights a
driven high; before each byte the board watches the clock for one bit and lets such a byte finish. A 330 Ω resistor in series
with SC on the port side is cheap insurance all the same.

With the 3DS key stored and no host on USB, the board runs the wrapper on its own (as with a GB-Link on the UART); with the
optional sense divider it waits for a Game Boy's 5 V and stops when the console has been off for 3 s. To watch it from a PC,
`uds-gblink-relay --port [--trace <file>]` (this repository's `tools/`) says HELLO, which takes the board back, and starts the wrapper with
its log.

## Flashing a pre-built firmware (no ESP-IDF needed)

Each release ships one file per build (bootloader, partition table and app merged):
`esp32-uds-bridge-fw<version>-esp32s3-gblink.bin` for a board on the PC's or phone's USB (Azahar, mGBA, a GB-Link through
uds-gblink-relay), and `esp32-uds-bridge-fw<version>-esp32s3-vcadapter.bin` for the VC-Adapter board on the console's link port.
Images from before 1.8 (`esp32-uds-bridge-fw1.7-esp32s3.bin`) are the GB-Link build. Either file is written at
**offset `0x0`**, gaps included, so it also **erases the stored 3DS key**: store the key again after flashing. Plug the board
in through its **native USB port** (the one that shows up as an Espressif USB Serial/JTAG device) and use one of these:

**With uds-esp32-setup (Windows).** `uds-esp32-setup.exe` ships next to the `.bin`. It
flashes the firmware and then stores the 3DS key from your `aes_keys.txt` in one go (either step can
be done on its own): pick **Build for** (GB-Link or VC-Adapter board; the newest `.bin` of that build beside it is filled
in, and it asks before flashing an image of the other build), your `aes_keys.txt` and the board, and press Start. Only the one
16-byte key the board needs (slot 0x2D) is sent to it, and it is never shown or logged. It is built from this repository's
`tools/` and needs nothing else installed.

**In the browser (nothing to install).** Open Espressif's [esptool-js](https://espressif.github.io/esptool-js/)
in Chrome or Edge (it needs Web Serial). Connect, pick the board's port, add the `.bin` at address
`0x0`, and Program.

**With esptool.**

```
pip install esptool
esptool --chip esp32s3 -p COM4 write_flash 0x0 esp32-uds-bridge-fw1.4-esp32s3.bin
```

(replace `COM4` with the board's port; on older esptool versions the command is `esptool.py`).

If the board is not detected, hold **BOOT** while plugging it in, then release it. The firmware prints
nothing after flashing; in Azahar's Network settings, **Connect** should report the firmware version.
The merged file was verified by flashing it to a real ESP32-S3 with esptool and with esptool-js.

To produce the merged files for a new release, run `package-release-bin.ps1` from an ESP-IDF PowerShell: it builds both
(`-Board gblink` or `-Board vcadapter` for one), each in its own `build-<board>` folder, and writes them to the repository's
`release/`. The build's name is also in the app's version (`1.8-vcadapter`), which uds-esp32-setup reads, and the firmware says
it in HELLO_ACK's features byte (1.8 on).

## Build and flash

Requires ESP-IDF 5.2 or newer (`idf.py`).

```
cd firmware/esp32-uds-bridge
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash        # COMx = the board's port on your PC
```

That builds the GB-Link build (`sdkconfig.defaults`). The VC-Adapter board build, in a folder of its own:

```
idf.py -B build-vcadapter -D SDKCONFIG=build-vcadapter/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.vcadapter" build
idf.py -B build-vcadapter -D SDKCONFIG=build-vcadapter/sdkconfig -p COMx flash
```

Flash it over the same native USB port (hold BOOT while plugging in if the port is not detected).
The firmware prints nothing: the USB port carries the binary protocol below, and `sdkconfig.defaults`
turns the console off.

## Host tests

The framing is shared with the emulator (`src/core/hle/service/nwm/uds_real/esp32_wire.*`) and both
sides are tested against the same golden vectors:

```
cd test
gcc -I../main wire_test.c ../main/uds_wire.c -o wire_test && ./wire_test
```

The emulator's copy is the Catch2 case `ESP32 wire` in `src/tests/core/hle/service/nwm/esp32_wire.cpp`.

## Protocol

Frame: `| version=1 | type | seq | flags | length:u16 LE | payload | crc32:u32 LE |`, CRC-32/ISO-HDLC over
everything before it, COBS encoded, terminated by `0x00`. A leading `0x00` at boot lets the host
discard the ROM boot banner. Maximum payload 2432 bytes.

Host to device:

| Type | Payload | Reply |
| --- | --- | --- |
| `0x01` HELLO | none | HELLO_ACK |
| `0x02` START | `channel:u8, mac[6]` (radio up with this MAC) | STATUS |
| `0x03` STOP | none | STATUS |
| `0x04` SET_CHANNEL | `channel:u8` | STATUS |
| `0x06` TX_FRAME | `tx_flags:u8, rate500k:u8, mpdu` (no FCS; rate 0 = 1 Mbit/s, 22 = 11 Mbit/s) | none (see `tx_failed` in STATS) |
| `0x07` SET_BEACON | beacon MPDU template; empty clears | STATUS |
| `0x08` SET_WATCH | `mac[6]`; all zero clears | STATUS |
| `0x09` PING | `token:u32` | PONG |

Firmware 1.4 adds a second job, the **Game Boy wrapper** (`main/gbwrap.c`, see `main/gbwrap/README.md`): the board itself joins a
3DS running a Virtual Console Pokemon game (Red, Blue, Yellow, Gold, Silver, Crystal) and stands in for a link-cable partner, so the
host only carries a cartridge's serial transfers. Azahar never sends these commands and keeps the raw radio above; while the wrapper
runs, the raw radio commands are refused (`ESP_ERR_INVALID_STATE`) and received frames stay on the board.

| Type | Payload | Reply |
| --- | --- | --- |
| `0x0A` SET_KEY | `slot:u8 (0x2D), key[16]` | STATUS. Stored in flash (NVS), never sent back. `idf.py flash` leaves it; a release image written at 0x0 erases it |
| `0x0B` KEY_STATUS | none | `0x88` KEY_INFO `{slot, present}` |
| `0x0C` ERASE_KEYS | none | STATUS |
| `0x10` GB_START | `flags:u8 (bit 0: log, bit 1: Pico, bit 2: detect the generation, bit 3 (1.8): the board's own link port), title[16] (cartridge header at 0x134), name[20] (UTF-16LE)` | STATUS: 0, `0x105` no key stored, `0x106` unknown game, `0x103` no link port built in |
| `0x11` GB_XFER | `master:u8`, the byte the cartridge clocked out | `0x91` GB_REPLY `{slave:u8, wire phase:u8}`, the byte shifted back in the same transfer |
| `0x12` GB_STOP | none | STATUS (STOP does the same while the wrapper runs) |
| `0x13` GB_PICO | a frame of the GB-Link Pico's protocol, from the Pico (1.6) | none |

Firmware 1.6 adds the **GB-Link model** (GB_START flag bit 1): the cartridge is the slave behind a GB-Link Pico in Game Boy mode,
whose SPI only clocks, and the board drives the clock with the same rules mGBA's Local mode runs (`main/gbwrap/uds-clock.c`). The
board speaks the Pico's own serial protocol (GB-Link GBLink-Firmware 2.2.7: `| 47 42 | channel | length:u16 LE | payload |`, channel
0 commands, 1 data, 2 status) inside `0x93` GB_PICO frames to the host and GB_PICO commands from it: SetMode Game Boy (`00 02`)
until the Pico reports `0xFF0A`, SetTimingConfig with no wait of its own (`30 00 00 00 01`), then one byte to clock per data frame,
the byte the Pico shifted in coming back. mGBA's Wireless Adapter > ESP32 stands in for the Pico over USB; a real Pico on the board's
UART takes the same frames. Without the flag the cartridge is the master, as in 1.4 and 1.5.

Firmware 1.7 makes the GB-Link model work with a **real cartridge**. The supported setup has no wires between the boards: a
GB-Link (GBLink-Firmware 2.2.7 or later) and this board both plug into a PC's USB, and mGBA's `uds-gblink-relay` carries the Pico's
frames between them, as GB-Link's own web page does for their firmware (the GB-Link's USB serial port speaks the same frames,
vendor id 2FE3):

```
uds-gblink-relay [--esp32 COMx] [--gblink COMy] [--key <aes_keys.txt>] [--gen 1|2] [--name NAME] [--trace <file>]
```

Nobody can read the header of a cartridge in a Game Boy, so GB_START's bit 2 (with bit 1) has the board take the generation from
the cartridge's first sync: the Cable Club receptionist syncs 60 in Gen 1 and 61 in Gen 2 (Time Capsule included), and a 70 or 80
range is Gen 2's alone. The screen shows G? until then. mGBA sends the same flag with `MGBA_VCLINK_GEN=auto`, standing in for such a
cartridge. Before Game Boy mode the board sends the Pico SetVoltage 5 V (`41`), as GB-Link's web page does for Gen 1 and 2 (an
official GB-Link sets the link port's voltage in software; the DIY board's SW1 must be on 5 V). In idle, sync and menu the gap after
each byte is what is left of a 12.6 ms period after the measured round trip, so a relay's milliseconds through the PC do not make
the clock miss a frame.

Optional, off by default (`idf.py menuconfig` > UDS bridge): a GB-Link wired to the board's UART with three wires, as GB-Link's
firmware also allows (UART 1 at 921600, GPIO2 to the Pico's GP9 and GPIO1 from its GP8). With it, and no host on USB, the board asks
the UART for Game Boy mode once a second and starts the wrapper on its own when a Pico answers; a host's HELLO takes the board back.

While the wrapper runs the board also sends `0x90` GB_STATE `{room, session, wire phase, generation, channel}` on every change,
`0x92` GB_STATS (10 × u32: beacons, frames sent and received, frames dropped for decryption, replay and other reasons, tx failures,
units sent and received, transfers) every 5 s, and, with the log flag, LOG lines. The key is the 3DS UDS data key, AES key slot `0x2D`
KeyN (the line `slot0x2DKeyN=` of Azahar's `aes_keys.txt`). mGBA stores it from its key file the first time it needs it, or by hand:
mGBA's probe tool (in mGBA's build folder; in PowerShell the quoted path needs `&` in front):

```
& "<mGBA build folder>\uds-esp32-probe.exe" COMx --store-key "<path>\aes_keys.txt"
```

and `--key-status` or `--erase-keys` in place of `--store-key <file>`.

Device to host: `0x81` HELLO_ACK `{proto, fw_major, fw_minor, factory_mac[6]}`, `0x82` STATUS
`{request_type:u8, result:s32}` (echoes the request's `seq`), `0x83` RX `{channel, rssi, flags, mpdu}`,
`0x85` STATS every 5 s (`rx_seen, rx_forwarded, rx_dropped, tx_ok, tx_failed, beacons_sent,
usb_dropped`), `0x86` PONG.

## Status: what has and has not been verified

Verified on the development PC: the framing (C and C++ agree with an independent Python reference),
and the emulator-side code compiles for both Windows and Android (arm64, NDK 26).

**Not verified: this firmware has not been built with ESP-IDF or run on a board.** The parts most
likely to need a first-hardware fix are the ones that depend on the Wi-Fi driver's behaviour:

Update: the board has since been run against a retail 3DS on a PC (ESP-IDF 5.2.8): discovery, both
join directions (Azahar hosting and Azahar joining) and trading data work. That run found that
hardware ACK generation and delivery of frames addressed to the board's own MAC are mutually
exclusive on this chip, hence the twin-MAC design above. Still untested: Android (USB permission,
the app's JNI link), and long sessions.

Things to look at first if something misbehaves:

1. **Peer retransmissions.** Without hardware ACKs the peer resends frames it thinks were lost; a
   lot of `retry=true` in the log means the join is slow.
2. **The raw-frame sanity-check override.** `-zmuldefs` and the symbol name are driver-version
   specific; a link error or `ESP_ERR_INVALID_ARG` from `esp_wifi_80211_tx` for authentication frames
   points here.
3. **Per-frame rate.** `esp_wifi_config_80211_tx_rate` needs ESP-IDF 5.1 or newer; older versions
   transmit at the default rate.
