# GBA Wireless Adapter on the link port

The Switch side's GBA end on the VC-Adapter board, with no GB-Link: the board itself is the Wireless Adapter the GBA game
talks to (`CONFIG_UDS_RFU_ADAPTER`, on in the VC-Adapter profile). GB-Link Switch LDN's bridge (`../ldn`) is unchanged but
for `pico_link.c`, which hands the Pico's 'GB' frames to this adapter instead of a UART.

| File | What |
| --- | --- |
| `rfuProtocol.hpp` | The adapter's protocol, GBLink-Firmware's `RfuCore` and RFU1 framing, copied unchanged from [GB-Link/GBLink-Firmware](https://github.com/GB-Link/GBLink-Firmware) `src/sections/rfuProtocol.hpp` at `v2.2.7` (`1b49b76`, branch `Wireless-Adapter`). Itself a port of gpsp's `rfu.c`. |
| `rfu_adapter.cpp`, `.h` | The ESP32-S3 side: a port of that firmware's `rfuProtocolSection.cpp` (the section thread) and `linkLayer_rfu.c` (the PIO link layer). |

Licence: GPL-3.0, as GBLink-Firmware. The firmware image that contains it also contains `../ldn` and is distributed under the
AGPL-3.0.

## How the link works here

The GBA clocks the adapter at 2 MHz while it sends commands (librfu's `SIO_115200_BPS`, SIO32 normal mode, internal clock)
and at 256 kHz during the ID check; the adapter clocks the GBA, at ~154 kHz here as on the Pico, to deliver an event after a
wait command. The Pico follows the 2 MHz clock with PIO. Here the second core does it in software: while the adapter is on,
its link task never leaves that core and runs with interrupts masked, reading SC, SO and SD and driving SI through the
ESP32-S3's dedicated GPIO instructions (a cycle each; the GPIO registers are too slow for a 238 ns half bit).

Software cannot listen while it works, but librfu leaves room for the work. After every word of a command, the GBA waits in a
handshake for the adapter's SI to answer its SO, and the adapter decides when it answers. That window is where frames from
the Switch side are applied and the core's timers run. Only the first word of each command, and every word of the ID check,
arrives unannounced, and nothing else runs then. While the GBA waits for an event it does not clock at all.

The second core lets interrupts in for a moment at most once a millisecond, whenever the GBA cannot start a transfer then: in
the handshake after each word, while the GBA waits for an event, in a delivery's handshakes, and when the GBA has been silent
for 50 ms. It has to: before every flash access (NVS reads included) the other core parks this one with an `esp_ipc` call and
waits, scheduler stopped, until it has answered. The first test build only let interrupts in during quiet spells, which froze
Wi-Fi and the Switch session for as long as the GBA kept talking: a GBA's connect timed out while the Switch side sat in a key
read. The VC-Adapter profile turns off the interrupt watchdog's check of that core and the task watchdog's wait for its idle
task.

Pins: the link port's (`CONFIG_UDS_LINK_PORT_*`): SC D0, the console's SO on D1, the console's SI on D2, SD on D3. The
console must be in GBA mode (3.3 V). The XIAO's user LED (GPIO21, `CONFIG_UDS_RFU_STATUS_LED_GPIO`) shows how far the GBA
got:

| LED | |
| --- | --- |
| off | adapter off (the bridge has not asked for it) |
| a blip every 2 s | waiting for the GBA: nothing clocked yet |
| slow blink | the GBA clocks it, but no ID word gets through (wiring, timing) |
| fast blink | ID words arrive, the ID check has not finished |
| on | the GBA found the adapter; off for a moment every second while the Switch side sends |

The telemetry the Pico sends twice a second (frames 0x0E, 0x2E, 0x1D, 0x1E) is sent the same way, so the bridge's
`LDN_PICO_STATUS` and session logs read as with a GB-Link.

## Differences from the Pico

- Frames from the Switch side wait in the inbound ring until it is safe to apply them, in order. The Pico parks them in a
  32-slot list, where a newer idle state from the parent can replace an older one.
- While the GBA clocks, SI changes right after the rising edge on which the GBA sampled the previous bit, not on the falling
  edge. Through the BSS138 level shifter a rising SI only climbs on its 10k pull-up, too slowly for half a 2 MHz bit: the
  first hardware test had the GBA misread acknowledgements and restart commands until it gave up with a communication
  error. SC's rise reaches the board well after the GBA latched SI, so the early change is safe.
- The per-word path runs from IRAM: the link loop, the core's command, frame and timer code (by symbol, in
  `../components/ldn_bridge/linker.lf`). The GBA's game loop stands still in librfu's serial interrupt until the adapter
  answers each word; from flash, the cache misses the other core's Wi-Fi code causes held it 60 µs and more a command, which
  halved the GBA's frame rate in the first room tests (~11 KB of internal RAM).
- How long the GBA was held after each word is reported in the 0x1E telemetry: the longest since the last report in 4 µs
  units (the bridge's log calls it `park_high`) and how many holds went past the Pico's 30 µs budget (`park_idle_drops`).
- Every bit the GBA sends, bit 0 included, is read once SC's rise has reached the board. GB-Link's own ESP32 adapter,
  wired straight to the port, takes the last bit before the GBA drops SO to idle at the word's end; through this board's
  level shifter a rising SO is not yet high there, and reading it early turned command 0x3D into 0x3C in a hardware test.
  SC, SO and SD go through the GPIO glitch filter.
- A wait for the GBA's clock that runs out goes straight back to waiting, with no timer read, until the GBA has been
  silent 50 ms. A word that starts while the loop is away is lost, and with it the word count: every later reply goes one
  word late. Hardware tests lost a word of a search result that way, in the microsecond or two the loop took to read the
  timer each time a wait ran out (holding the wait open while SO was high did not help: SO rests high between commands).
- The wait for a word looks at SC between every step, and the first bit has no blank after its falling edge. Through the
  level shifter SC's rise climbs on the 10k pull-up and reads high only briefly at 2 MHz; noticing the first fall a loop
  late and then blanking missed bit 31 of a search result word in a hardware test.
- Between commands the wait for SC runs in one go until the GBA has been silent 50 ms, instead of returning to the loop every
  100 us: a command's first word arrives unannounced, and a hardware test lost the first bit of ~2% of command headers
  (librfu then waits 130 ms before it retries, which backed up the Switch's data until frames were dropped and the game
  reported a lost link).
- A word lost mid-way in the command phase is counted in the 0x1E telemetry's parked-loss field (the bridge's log calls
  it `park_drops`) and goes into the transfer recorder (comstate 15), whose first one is sent like a reset's. An SC that
  falls and stays low (no bit at all, as around the GBA's soft reset) is not counted.
- The recorder is also sent when the GBA has been silent for a second in the command phase (reason 0), which shows the
  last words before a game froze.
- A delivery is never retried: the Pico retries when its state machine never clocked the first word, which cannot happen to
  a software clock.
- The soft reset on SD counts once the pulse has ended (high 0.4 to 3 ms, then low). The Pico counts the rising edge once SD
  has stayed high 40 µs.
- `LDN_PICO_PROBE` and `LDN_PICO_SWAP` do nothing: the bridge's UART pins are link port pins on this board.
