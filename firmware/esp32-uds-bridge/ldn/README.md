# GB-Link Switch LDN bridge (vendored)

The Switch side of the merged firmware: GB-Link Switch LDN's ESP32 bridge firmware, `firmware/common/` of
[GB-Link/GB-Link-Switch-LDN](https://github.com/GB-Link/GB-Link-Switch-LDN) at commit `03a54d1` (2026-11-15, bridge 2.2.4),
which has the `esp-adapter` work merged: the Union Room both ways, battles and multi battles, groups of more than two, the
hosted room moving to where a Switch is heard, frames relayed between stations, rooms looked for only while the GBA looks
for a group (with GB-Link firmware that reports its searches), the bridge starting over without a restart whenever the
adapter is wired to the board, and the port's pin 1 read through a divider (`gba_vcc.c`, off unless
`CONFIG_BRIDGE_GBA_VCC_GPIO` is set; the VC-Adapter board reads pin 1 with `main/port_watch.c` instead). Their own on-board
adapter (`firmware/components/rfu_adapter`, on the chip's SPI slave, chosen by `firmware/main/pico_link_select.c`) is not
used: this board keeps its own (`../rfu`), which has Ruby and Sapphire's cable and has been run on this board. It joins or
hosts a Nintendo Switch local wireless (LDN) room and carries a GBA game's wireless-adapter traffic to it. See their
`firmware/README.md` for what it does and its console commands.

Licence: AGPL-3.0 (`LICENSE`); the LDN protocol components are GPL-3.0 (`LDN-GPL-3.0.txt`). The merged firmware that
includes these files is distributed under the AGPL-3.0.

## Changes for the merge

Kept as close to upstream as possible, so that a newer upstream can be copied over again. Every change is marked
`VC-Adapter merge` in the source:

- `ldn_probe.c`: `app_main` is renamed `ldn_app_main`. The merged firmware's own `app_main` (`main/main.c`) picks the mode
  the board boots in and calls it for Switch LDN mode.
- `ldn_control.c`: every byte the host sends, in text and binary mode alike, is also handed to `bridge_usb_sniff` (a weak
  no-op here; `main/bridge_mode.c` provides the real one), which notices a 3DS host's HELLO and restarts the board in 3DS mode.
  No frame of this firmware's own can be mistaken for one. (It used to stop at binary mode, so a board whose Switch host had
  set the wire up and then closed the port stayed in Switch LDN mode until it was power cycled.)
- `transport.h`, `transport_usb_serial_jtag.c`, `transport_uart.c` (2.0): the merged firmware's 3DS side uses the same host link,
  so that it runs on the classic ESP32's UART too. Three calls are added for it (`bridge_transport_open` with its own buffer
  sizes and no stdio hook, `bridge_transport_read_wait`, `bridge_transport_write_wait`); the UART's driver install takes its
  ring sizes as an argument; and the `_vfs` headers are included only where they exist (ESP-IDF 5.x builds).
- `ldn_keys.c` (2.0): `ldn_decode_advertisement`'s `material` buffer was 1312 bytes, 40 short of the 72 + 1280 copied into it
  and hashed (a stack overrun in the 1280-byte advertisement branch; RISC-V GCC at `-O2` reports it on the C3 and C6). It is
  `72 + 1280` now. Still so upstream at `03a54d1`.

- `pico_link.c`: with `CONFIG_UDS_RFU_ADAPTER` (the VC-Adapter board), the adapter is not a GB-Link Pico on the UART but
  the Wireless Adapter emulated on the board's own link port pins (`../rfu`): the 'GB' frames go to `rfu_adapter_write`
  and come from `rfu_adapter_read`, the link task runs on the first core (the second belongs to the adapter's link), and
  `pico_link_probe` / `pico_link_swap` leave the pins alone, and the inbound frame queue is 24 frames instead of 48 (the
  adapter's link code needs the internal RAM). Without the option the file builds as upstream's.
- `pico_link.c`: `pico_link_on_board` answers true with the emulated adapter (`LDN_PICO_ADAPTER board`). Upstream starts the
  bridge over in place (their `restart_in_place`) for any adapter but a host-port client's, so this board never restarts after
  a session or a failed join: a restart would take the adapter on this chip away from a GBA that may be waiting on it, as one
  leaving the trade room is (a hardware test ended on a black screen that way). `pia_bridge.c` is theirs, unchanged. The
  emulated adapter sends no search reports, so `pico_link_gba_search_age_ms` answers -1 and the bridge looks for rooms as it
  did before those reports. Switching to the host port (`LDN_ADAPTER host`) stops the emulated adapter, as upstream stops its
  own while a client carries the session.
- `Kconfig.projbuild`: the GB-Link UART's pins keep their 2.1.5 defaults (GPIO2 to the GB-Link's GP9, GPIO1 from its GP8;
  `sdkconfig.defaults.<chip>` sets the classic ESP32's and the C3's). Upstream 2.2 moved them to each XIAO's D9 and D8, which
  would leave a board wired for the old defaults with no adapter.
- `ldn_keys.c`: the keys are read from flash once and kept in RAM (`ldn_keys_store` and `ldn_keys_erase` keep that copy in
  step). With the adapter on the second core, interrupts masked, every flash read on the first waits for that core to let
  them in, and each key derivation (three reads) took about 200 ms. A room the GBA leads then answered a Switch's
  authentication 400 to 800 ms late, after the Switch, which asks every 200 ms and gives up after five, had left
  ("The other trainer appears unavailable"). Both builds have the change.

Everything else, `components/zstd` included, is unchanged. The files are built as their own component,
`../components/ldn_bridge`, whose linker fragment puts all their static data in one block; when the board boots in 3DS mode
that block is handed to the heap (`main/bridge_mode.c`), since this code does not run then. Like upstream, it needs ESP-IDF
v6.1 at commit `fff9895c82d744c7237be8847347bdd1b07c6643` and checks the chip's Wi-Fi driver library hash (upstream's, for
the ESP32, C3, C6 and S3: `../components/ldn_bridge/CMakeLists.txt`); a build with ESP-IDF 5.x leaves these files out, but for
the host link, and has the 3DS side only.
