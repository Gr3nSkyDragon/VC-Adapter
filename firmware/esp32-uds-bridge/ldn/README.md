# GB-Link Switch LDN bridge (vendored)

The Switch side of the merged firmware: GB-Link Switch LDN's ESP32 bridge firmware, `firmware/common/` of
[GB-Link/GB-Link-Switch-LDN](https://github.com/GB-Link/GB-Link-Switch-LDN) at commit `8b47745` (bridge 2.1.5). It joins
or hosts a Nintendo Switch local wireless (LDN) room and carries a GBA game's wireless-adapter traffic to it. See their
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

Everything else, `components/zstd` included, is unchanged. The files are built as their own component,
`../components/ldn_bridge`, whose linker fragment puts all their static data in one block; when the board boots in 3DS mode
that block is handed to the heap (`main/bridge_mode.c`), since this code does not run then. Like upstream, it needs ESP-IDF
v6.1 at commit `fff9895c82d744c7237be8847347bdd1b07c6643` and checks the ESP32-S3 Wi-Fi driver library's hash
(`../components/ldn_bridge/CMakeLists.txt`); a build with ESP-IDF 5.x leaves these files out and has the 3DS side only.
