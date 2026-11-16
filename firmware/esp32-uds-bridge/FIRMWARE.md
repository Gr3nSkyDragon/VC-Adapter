# Firmware lines and chips (2.0)

esp32-uds-bridge 2.0 comes as two firmware lines, built from this one source tree:

| Line | Was | For | What it has | Chips |
| --- | --- | --- | --- | --- |
| **2.0-uds** | 1.8, profile 1, `gblink` | A board on a PC's or phone's USB: the bridge for **mGBA, Azahar and PokeUDS** (and a GB-Link through `uds-gblink-relay` or GB-Link's own clients) | The 3DS side and GB-Link Switch LDN's Switch side, switching by what the host speaks | ESP32-S3; **alpha:** classic ESP32, ESP32-C3, ESP32-C6 |
| **2.0-aio** | 1.9 (beta), profile 2, `vcadapter` | The standalone VC-Adapter board for the **GB/GBA games**, wired to the console's link port (`../../VC-Adapter-DIY`), no PC | The 3DS side and the link port, running on its own. Game Boy side only for now (beta) | ESP32-S3 only |

Both lines report firmware **2.0** in HELLO_ACK; its link-port feature bit tells them apart, as it did between 1.8 and 1.9. The
line is also in the app's version (`2.0-uds`, `2.0-aio`, which uds-esp32-setup reads) and in the image's name:

```
esp32-uds-bridge-2.0-uds-esp32s3.bin
esp32-uds-bridge-2.0-uds-esp32.bin      (alpha)
esp32-uds-bridge-2.0-uds-esp32c3.bin    (alpha)
esp32-uds-bridge-2.0-uds-esp32c6.bin    (alpha)
esp32-uds-bridge-2.0-aio-esp32s3.bin    (beta)
```

Each is merged (bootloader, partition table, app) and written at offset **`0x0`**, gaps included, so flashing erases the stored
keys. Inside the classic ESP32's image the bootloader sits at `0x1000` (that chip's ROM looks for it there); on the other three it
sits at `0x0`. `package-release-bin.ps1` (Windows) and `package-release-bin.sh` (Linux, macOS, WSL) build them all with ESP-IDF
v6.1 installed for the selected chips; `-Line uds|aio` and `-Chip <chip>` (or `./package-release-bin.sh <line> <chip>`) build one.

## What is alpha

**Only the ESP32-S3 has run this firmware.** The classic ESP32, C3 and C6 images build and link cleanly with ESP-IDF v6.1
(`fff9895c`) and pass the static checks below; nothing else about them is known. Treat every figure for those chips in this
file and in `sdkconfig.defaults.<chip>` as **unmeasured** until it has been seen on a board.

The S3's 2.0-uds is the 1.8 firmware with three changes: its version and name, `CONFIG_ESP_SYSTEM_BBPLL_RECALIB=n` (below), and
the vendored `ldn/ldn_keys.c` fix (a 40-byte stack overrun upstream, below). Its other settings came out identical to 1.8's
(sdkconfig compared), and its image is within 40 bytes of 1.8's. **2.0 on the S3 is unmeasured too** until it has been flashed
and a trade done; 1.8's measurements (README, "On hardware") carry over only as far as these changes leave them.

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
- uds-esp32-setup (this repository's `tools/`) knows the 2.0 names, but still flashes ESP32-S3 boards only.

## Sources

The per-chip settings, the Wi-Fi driver hashes and the XIAO C6 board setup (`main/board.c`) come from
[GB-Link/gb-link-switch-ldn](https://github.com/GB-Link/gb-link-switch-ldn) at commit `c972db4` (AGPL-3.0), whose `firmware/`
builds the same vendored `ldn/` for five chips. The `BBPLL_RECALIB` setting and the CP210x baud figures come from
[Decryptu/pokeldn](https://github.com/Decryptu/pokeldn)'s `docs/hardware_esp32.md` (AGPL-3.0); no pokeldn code is copied here.

## ESP32-S2 with native USB CDC

The experimental Flipper Wi-Fi Developer Board build uses the `2.0-uds` release format.
The USB OTG port runs TinyUSB CDC in slave mode to avoid the ESP32-S2 USB-105 erratum.
This target starts directly in 3DS radio mode, with smaller Wi-Fi buffers and no on-board
Game Boy wrapper or OLED profile. PokeUDS handles the game protocol on the computer.

Build the merged release image with ESP-IDF 6.1 and its ESP32-S2 tools installed:

```sh
./package-release-bin.sh uds esp32s2
```

To enter download mode, hold BOOT, press and release RESET, then release BOOT. The merged
`esp32-uds-bridge-2.0-uds-esp32s2.bin` writes at offset `0x0`; its bootloader is at
`0x1000` inside the file.

The standalone `2.0-uds` image was tested on a Flipper Wi-Fi Developer Board (ESP32-S2
rev 0.0) with arm64 macOS. USB HELLO passed after a physical cable replug. Wi-Fi initialized,
scanned channels 1, 6, and 11 for 20 seconds, and answered HELLO again afterward. The driver
received 1,781 Wi-Fi frames with no USB drops or malformed USB frames. No 3DS hosts were
heard. Retail 2DS/3DS discovery, encrypted communication, and trading remain untested.
