# VC-Adapter-DIY

**Proof of concept. The PCB is unfinished and has not been fabricated or tested.** The circuit itself has been proven on a
breadboard (see below); the board layout is a work in progress.

A KiCad 10 project for the VC-Adapter: a Seeed XIAO ESP32-S3 plugged straight into a GBA, GBC or Game Boy's link port, with no
GB-Link (Pico) and no PC. It runs firmware **1.9 (beta)**, the VC-Adapter board build of `../firmware/esp32-uds-bridge`, which
clocks the cartridge itself and trades with a 3DS's Virtual Console Pokemon games over the 3DS's local wireless. The Game Boy
side only for now: Gen 3 (GBA) games need the board's own GBA side, which is still to come.

## Circuit

| Part | What |
| --- | --- |
| U1 | Seeed XIAO ESP32-S3 (DIP) |
| U2 | SparkFun BOB-12009 bidirectional level shifter (BSS138; clones work) |
| J1 | GBA link port plug (`gba_linkport/gba_linkport.pretty/GBALinkPort_Male`), going straight into the console |

The link port's signals are 5 V when a GBA runs a Game Boy cartridge (3.3 V for a GBA game) and the ESP32's pins take 3.3 V, so
the shifter's high side runs from the port's own VCC (pin 1) and follows the console; its low side runs from the XIAO's 3V3. No
resistors are needed. The pinout is the one the firmware expects (`sdkconfig.vcadapter`, the shared VC-Adapter / GB-Link
pinout); SI and SO are the adapter's, as in the GB-Link's notation:

| Plug pin (console's signal) | Shifter | XIAO ESP32-S3 |
| --- | --- | --- |
| 1 (VCC) | HV | - |
| 2 (SO, the console's output) | HV1 / LV1 | D1 = GPIO2, the adapter's SI |
| 3 (SI, the console's input) | HV2 / LV2 | D2 = GPIO3, the adapter's SO |
| 4 (SD) | HV4 / LV4 | D3 = GPIO4 (unused by a Game Boy) |
| 5 (SC, the clock) | HV3 / LV3 | D0 = GPIO1 |
| 6 (GND), plug shield | GND (both sides) | GND |
| - | LV | 3V3 |

A plug that goes into the console has pins 2 and 3 this way round; a socket with a link cable to the console would have them
the other way (the cable crosses SO and SI).

## Status

- Breadboard (XIAO ESP32-S3, BOB-12009, a GBA link plug, wired as above): retail Pokemon Red traded with Virtual Console Blue
  on a 3DS through firmware 1.9, with the board on its own and with a PC watching (`uds-gblink-relay --port`).
- PCB: unfinished. Not yet on it: the optional sense divider (two 100 kOhm resistors from pin 1 to GND, the middle to D4 =
  GPIO5, plus 100 nF), with which the firmware can tell a Game Boy game (5 V) from a GBA game (3.3 V) or no console, and a
  battery and its switch (the XIAO charges a single-cell LiPo on its BAT pads from USB-C).

## Licence

MIT (`LICENSE`). `gba_linkport` is MIT from GB-Link-USB-DIY (its own `LICENSE`).
