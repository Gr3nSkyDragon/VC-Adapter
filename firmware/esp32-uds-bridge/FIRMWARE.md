# Firmware lines and chips (2.1)

esp32-uds-bridge 2.1 comes as three firmware lines, built from this one source tree:

| Line | Was | For | What it has | Chips |
| --- | --- | --- | --- | --- |
| **2.1-uds** | new in 2.1 | A board on a computer whose host only speaks 3DS: **Azahar, PokeUDS** | The 3DS side alone (`sdkconfig.uds`): it boots in 3DS mode, so the first HELLO never restarts it and the board's USB port never drops (on macOS a dropped port can come back under another name) | ESP32-S3, ESP32-S2; **alpha:** classic ESP32, ESP32-C3, ESP32-C6 |
| **2.1-ldn** | 2.0-uds; 1.8, profile 1, `gblink` | A board on a PC's or phone's USB for **mGBA-LDN** (and a GB-Link through `uds-gblink-relay` or GB-Link's own clients) | The 3DS side and GB-Link Switch LDN's Switch side, switching by what the host speaks: a restart each way | ESP32-S3; **alpha:** classic ESP32, ESP32-C3, ESP32-C6 |
| **2.1-aio** | 2.0-aio; 1.9 (beta), profile 2, `vcadapter` | The standalone VC-Adapter board for the **GB/GBA games**, wired to the console's link port (`../../VC-Adapter-DIY`), no PC | The 3DS side and the Switch side on the link port, running on its own: Game Boy games through the 3DS side, FireRed, LeafGreen and Emerald through the GBA Wireless Adapter emulated on the port (`rfu/`), Ruby and Sapphire through their link cable (`cable/`) | ESP32-S3 only |

**The name `uds` changed meaning in 2.1.** 2.0-uds had both sides; that line is 2.1-ldn now, and 2.1-uds is the 3DS side alone.
Azahar and PokeUDS work on either (2.1-ldn restarts once into 3DS mode when they connect, as 2.0-uds did).

Every line reports firmware **2.1** in HELLO_ACK, and its features byte tells them apart: the link-port bit (`0x01`) marks aio, as it
did between 1.8 and 1.9, and the no-Switch bit (`0x04`, `UDS_HELLO_FEATURE_NO_SWITCH`) is set whenever the image has no Switch side:
the uds line, and aio images from before it had the GBA side. Neither bit: ldn (and aio with the GBA side, which the link-port bit
marks). Firmware 2.0 and older never set `0x04`. The line is also in the app's version (`2.1-uds`, `2.1-ldn`,
`2.1-aio`, which uds-esp32-setup reads) and in the image's name:

```
esp32-uds-bridge-2.1-uds-esp32s3.bin
esp32-uds-bridge-2.1-uds-esp32.bin      (alpha)
esp32-uds-bridge-2.1-uds-esp32c3.bin    (alpha)
esp32-uds-bridge-2.1-uds-esp32c6.bin    (alpha)
esp32-uds-bridge-2.1-uds-esp32s2.bin    (the uds line only)
esp32-uds-bridge-2.1-ldn-esp32s3.bin
esp32-uds-bridge-2.1-ldn-esp32.bin      (alpha)
esp32-uds-bridge-2.1-ldn-esp32c3.bin    (alpha)
esp32-uds-bridge-2.1-ldn-esp32c6.bin    (alpha)
esp32-uds-bridge-2.1-aio-esp32s3.bin
```

Each is merged (bootloader, partition table, app) and written at offset **`0x0`**, gaps included, so flashing erases the stored
keys. Inside the classic ESP32's and the S2's image the bootloader sits at `0x1000` (those chips' ROM looks for it there); on the
other three it sits at `0x0`. `package-release-bin.ps1` (Windows) and `package-release-bin.sh` (Linux, macOS, WSL) build them all with ESP-IDF
v6.1 installed for the five chips; `-Line uds|ldn|aio` and `-Chip <chip>` (or `./package-release-bin.sh <line> <chip>`) build one.

## What is alpha

**The ESP32-S3 and, since 2026-11-20, the ESP32-S2 (its uds line, below) have run this firmware.** The classic ESP32, C3 and C6 images build and link cleanly with ESP-IDF v6.1
(`fff9895c`) and pass the static checks below; nothing else about them is known. Treat every figure for those chips in this
file and in `sdkconfig.defaults.<chip>` as **unmeasured** until it has been seen on a board.

The S3's 2.0-uds is the 1.8 firmware with three changes: its version and name, `CONFIG_ESP_SYSTEM_BBPLL_RECALIB=n` (below), and
the vendored `ldn/ldn_keys.c` fix (a 40-byte stack overrun upstream, below). Its other settings came out identical to 1.8's
(sdkconfig compared), and its image is within 40 bytes of 1.8's. **2.0 on the S3 is unmeasured too** until it has been flashed
and a trade done; 1.8's measurements (README, "On hardware") carry over only as far as these changes leave them.

2.1 changes three things from 2.0: the version, the no-Switch feature bit, and the uds line (`sdkconfig.uds`, the Switch side
turned off, the same switch the aio line has always used). 2.1-ldn is 2.0-uds under its new name. All nine 2.1 images built
(2026-11-19, ESP-IDF v6.1 `fff9895c`) with no new warnings; the uds image links none of the Switch side's code. **None has run on
a board yet**: flash an S3 with 2.1-uds and check that HELLO answers at once with features `0x04` and that the port never drops.

## Per chip

| | ESP32-S3 | classic ESP32 | ESP32-C3 | ESP32-C6 |
| --- | --- | --- | --- | --- |
| Status | measured (1.8); 2.0 unmeasured | **alpha** | **alpha** | **alpha** |
| Host link | native USB Serial/JTAG (`303A:1001`) | **UART0** (GPIO1/3) behind the board's USB bridge (CP210x `10C4:EA60`, CH340 ...), **921600** baud from boot | native USB Serial/JTAG (`303A:1001`) | native USB Serial/JTAG (`303A:1001`) |
| CPU | 240 MHz, 2 cores | 240 MHz, 2 cores | 160 MHz, 1 core | 160 MHz, 1 core |
| 3DS side's Wi-Fi buffers (static RX / dynamic RX / dynamic TX) | 16 / 64 / 64 | 4 / 16 / 16 | 4 / 16 / 16 | 16 / 64 / 64 |
| 3DS side's frame ring, host-link buffers (RX / TX) | 48 KB, 16 / 16 KB | 16 KB, 4 / 8 KB | 16 KB, 8 / 4 KB | 48 KB, 16 / 16 KB |
| Switch side's settings | ESP-IDF defaults | upstream's `firmware/ESP32` (4/16/16, zstd context on the heap, 1 KB literal buffer, no IPv6) | upstream's `firmware/ESP32-C3` (4/16/16, Wi-Fi IRAM code in flash, 8/4 KB USB buffers, 8.5 dBm cap) | upstream's `firmware/ESP32-C6` |
| GB-Link Pico UART (TX to GP9, RX from GP8) | GPIO2, GPIO1 | GPIO17, GPIO16 | GPIO5, GPIO4 | GPIO2, GPIO1 |
| SSD1306 screen (SDA, SCL), BOOT | 8, 9; 0 | 21, 22; 0 | 6, 7; 9 | 22, 23; 9 |
| Board setup | | | Wi-Fi transmit power capped at 8.5 dBm on both sides (some C3 boards garble frames at full power) | XIAO ESP32-C6 RF switch: GPIO3 and GPIO14 driven low before Wi-Fi starts (`UDS_BOARD_XIAO_C6_RF_SWITCH`, on by default) |
| `CONFIG_ESP_SYSTEM_BBPLL_RECALIB` | `n` (new in 2.0) | default | default | `n` |
| Static data (build), of which the Switch side's block handed to the heap in 3DS mode | 270 KB of 342 KB; 164 KB | **162 KB of 181 KB**; 124 KB | 253 KB of 321 KB; 164 KB | 309 KB of 452 KB; 164 KB |

The per-chip memory numbers are Kconfig defaults (`main/Kconfig.projbuild`, menu "Memory (per chip)") and can be changed in
menuconfig. The 3DS side logs its free memory as LOG frames at boot, after `radio_init`, before and after `esp_wifi_init`, and on
every HELLO (`main/diag.c`): the first board of each chip should be read through these before anything else.

The classic ESP32 is the tight one: the 3DS side runs in what is left of 181 KB of DRAM after the Wi-Fi driver, plus the Switch
side's 124 KB block once handed back. Upstream's buffer counts for it were chosen for the Switch side, not for raw 3DS capture.

## Checked without hardware

- **Builds**: all four chips (2.0-uds) and the S3's 2.0-aio, no new warnings (the one left, `_destination` unused in
  `gbwrap/uds-session.c`, is in 1.8 too).
- **Pinned Wi-Fi drivers**: each chip's `libnet80211.a` matches upstream's SHA-256 for ESP-IDF v6.1 (the build checks it:
  `components/ldn_bridge/CMakeLists.txt`).
- **`ieee80211_raw_frame_sanity_check`** (`nm`, `objdump -r`): on all four chips it is a global function in `libnet80211.a`'s
  `ieee80211_output.o`, called there through a relocation against the symbol (Xtensa `R_XTENSA_32` on the S3 and classic ESP32,
  `R_RISCV_CALL` on the C3 and C6), and in each firmware ELF the symbol resolves to `main/radio.c`'s override (`-zmuldefs`). So
  the driver's own call should reach the override on every chip. Whether auth, assoc and beacon frames then really go out is for
  the air to say (below).
- **The classic ESP32's ADC calibration**: it has no curve fitting, so `linkport.c` falls back to line fitting
  (`ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED`). Compiled for the classic chip on its own; it only builds into the aio line, which is
  S3-only, so no image carries it today.

## On hardware (check these first)

None of these has been done. In order, on each chip:

1. **Boot and memory.** The board answers HELLO (2.0, link-port bit clear), and its LOG lines show free internal memory after
   `radio_init` and after `esp_wifi_init`. On the classic ESP32, through the USB bridge at 921600 baud, with the host leaving DTR
   and RTS alone (or the auto-reset circuit resets the board on open).
2. **USB after a reset (S3, C6).** With `BBPLL_RECALIB=n`, the port comes back clean after the reset that opening it causes
   (pokeldn saw it garbled with the default). New on the S3 in 2.0.
3. **Frame injection.** The `-zmuldefs` override replaces a function inside each chip's closed `libnet80211.a`: confirm on air
   (a sniffer, or the 3DS's answers) that authentication, association and beacon frames go out. The C6's Wi-Fi 6 driver may check
   frames on another path.
4. **The twin-MAC receive trick** (`radio.c`, first MAC octet XOR `0x02`) relies on the S3's receive filter. Confirm on the C6 (and
   the others) that the join handshake still sees its unicast frames: `txUnicast == acksRx` in the stats, and the join completes.
5. **Beacon steadiness.** The 102.4 ms `esp_timer` beacon, on a single core (C3, C6) under USB and receive load.
6. **Throughput on the classic ESP32.** 921600 baud carries about 92 KB/s each way. Watch `usb_dropped` and `rx_dropped` in the
   stats during a trade. pokeldn measured a CP2102 at 1.5 Mbaud with no loss and no answer at 2 and 3 Mbaud; a BAUD command (start
   at 115200, switch both ends) is not in this firmware.
7. **Memory under load (classic ESP32, C3).** 4/16/16 Wi-Fi buffers and the smaller rings, during a scan and a trade.
8. **The Game Boy wrapper's CCMP on 160 MHz (C3, C6).** If it cannot keep up, move `gbwrap`'s `udsAesEncrypt` to the hardware AES
   (PSA / mbedtls).
9. **The XIAO C6's antenna**: the RF switch is on (GPIO3 low) and the onboard antenna selected (GPIO14 low) in both modes.
10. Then a **scan, a Gen 6 trade and a Gen 7 trade** with PokeUDS, a VC trade through mGBA, and the Switch side's join, on each chip.

## Hosts

What a host has to do differently for the new chips:

- **USB ids.** The S3, C3 and C6 all show up as `303A:1001` (Espressif USB Serial/JTAG): the chip is told apart by esptool's chip
  detection, not by the id. The classic ESP32 shows up as its USB bridge: CP210x `10C4:EA60`, or CH340 `1A86:7523` and the like.
- **Baud.** Native USB ignores the rate. The classic ESP32's UART runs at **921600** from boot (the boot ROM prints its banner at
  115200 first; the firmware's first byte is a frame delimiter, after which the host drops the garbage).
- **DTR and RTS.** A USB bridge board wires them to EN and GPIO0 (auto-reset): open the port without pulsing them, or the board
  resets (or lands in its ROM loader).
- **Flashing.** Pick the image by the detected chip; all are written at `0x0`. A host that checks the image should expect the
  classic ESP32's bootloader header at `0x1000` of the file, the other chips' at `0x0`.
- uds-esp32-setup (this repository's `tools/`) knows the 2.1 names (and reads a 2.0-uds image as the ldn line), but still
  flashes ESP32-S3 boards only.
- **A board that restarts on HELLO** (the ldn line, 2.0-uds, 1.8) drops its USB port and enumerates again. A host should find
  it again by its USB serial number (the chip's MAC on the S3, C3 and C6), not by the port's old name: macOS can give it
  another `/dev/cu.usbmodem...`. The uds line never restarts, so it never comes up.

## Sources

The per-chip settings, the Wi-Fi driver hashes and the XIAO C6 board setup (`main/board.c`) come from
[GB-Link/gb-link-switch-ldn](https://github.com/GB-Link/gb-link-switch-ldn) at commit `c972db4` (AGPL-3.0), whose `firmware/`
builds the same vendored `ldn/` for five chips. The `BBPLL_RECALIB` setting and the CP210x baud figures come from
[Decryptu/pokeldn](https://github.com/Decryptu/pokeldn)'s `docs/hardware_esp32.md` (AGPL-3.0); no pokeldn code is copied here.

## ESP32-S2 with native USB CDC

The ESP32-S2 build (made on a Flipper Wi-Fi Developer Board, out of alpha after a Gen 6 trade on an S2 Mini) is in the **uds line only**: GB-Link Switch LDN has no
S2 build, so `UDS_SWITCH_LDN` cannot be turned on for this chip, and its HELLO_ACK sets the no-Switch bit (`0x04`) as 2.1-uds does
on the other chips. There is no S2 ldn image.
The USB OTG port runs TinyUSB CDC in slave mode to avoid the ESP32-S2 USB-105 erratum.
This target starts directly in 3DS radio mode, with smaller Wi-Fi buffers and no on-board
Game Boy wrapper or OLED profile. PokeUDS handles the game protocol on the computer.

Build the merged release image with ESP-IDF 6.1 and its ESP32-S2 tools installed:

```sh
./package-release-bin.sh uds esp32s2
```

To enter download mode, hold BOOT, press and release RESET, then release BOOT. The merged
`esp32-uds-bridge-2.1-uds-esp32s2.bin` writes at offset `0x0`; its bootloader is at
`0x1000` inside the file.

The standalone `2.0-uds` image was tested on a Flipper Wi-Fi Developer Board (ESP32-S2
rev 0.0) with arm64 macOS. USB HELLO passed after a physical cable replug. Wi-Fi initialized,
scanned channels 1, 6, and 11 for 20 seconds, and answered HELLO again afterward. The driver
received 1,781 Wi-Fi frames with no USB drops or malformed USB frames. No 3DS hosts were
heard. Retail 2DS/3DS discovery, encrypted communication, and trading remain untested.

On 2.1 (2026-11-20) the S2 image is built with `sdkconfig.uds`, as the other chips' uds images are, and the S2's Wi-Fi driver is
pinned (SHA-256 `df1a279d...`); in its ELF `ieee80211_raw_frame_sanity_check` resolves to `main/radio.c`'s override. TinyUSB's
port does not take esptool's DTR/RTS reset, so every flash starts from download mode (BOOT held at reset) and ends with RESET. Its USB serial number is TinyUSB's default, `123456`, on every S2, so it does not tell boards apart (the uds
line never restarts, so a host has no need to find it again). The TinyUSB components are pinned to exact versions in
`components/ldn_bridge/idf_component.yml`; the component manager's `dependencies.lock` is per target, so it is not kept.
On a board (2026-11-20): this 2.1-uds S2 image, installed by PokeUDS's Board page on a Lolin (Wemos) S2 Mini v1.0.0
(ESP32-S2FN4R2, 4 MB flash, 2 MB PSRAM), traded **Gen 6** with a retail 3DS through PokeUDS. Other generations, the
other 3DS hosts (Azahar) and the throughput under a busy room are not yet tried on an S2.
