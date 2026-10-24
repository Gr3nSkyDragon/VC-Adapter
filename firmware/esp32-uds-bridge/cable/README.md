# Ruby and Sapphire on the link cable (first hardware test)

Ruby and Sapphire have only the link cable; FireRed, LeafGreen and Emerald use the Wireless Adapter. With this folder an RS
cartridge on the VC-Adapter board trades with the Switch's FireRed room, as mGBA's RFU Cable Wrapper does in the emulator:
the board is the game's cable partner (player 0, the master) and joins the Switch's trade room as an FRLG child,
translating between the two link protocols (`CONFIG_UDS_RFU_CABLE`). Client side only, as in mGBA.

| File | What |
| --- | --- |
| `rfu-wrapper-air.c` | The wrapper's wireless side and translator, copied **unchanged** from mGBA's `src/gba/sio/rfu-wrapper-air.c` (mgba_LDN, commit `bb0125780`). |
| `compat/` | Stand-ins for the mGBA headers it includes, so it builds unchanged: the adapter event queue, the backend interface, the wrapper's peer, the cable trace. `rfu-wrapper-air.h` is mGBA's own. |
| `cable_air.c`, `.h` | The board's half of the wireless side: the event queue, and a backend that is the client half of mGBA's ESP32 backend (`rfu-esp32.c`), talking RFU1 frames to the bridge on this chip instead of over USB. |
| `cable_master.c`, `.h` | The cable's master, mGBA's `rfu-wrapper.c` driver logic without the emulator: the handshake, the checksum and the eight words of each packet. |
| `test/` | PC tests: `cable_air_test.c` (the wireless side against a fake bridge) and `cable_master_test.c` (the master and the wireless side against a fake Ruby, to the join request). |

The wire itself is in `../rfu/rfu_adapter.cpp` (`cableExchange`, `probeCable`, `cableSession`), next to the adapter it shares
the link port and the second core with. A fix made in mGBA's file can be copied over as it is.

Memory: the wireless side's state (about 8 KB, mGBA's `struct Air`) lives in the adapter core's memory, which is idle while the
cable runs; the event queue (8 entries here, against mGBA's 64) and the cable's log are static.

Licence: MPL-2.0 (these files, as mGBA's), which allows the AGPL-3.0 firmware image that contains them.

## On the wire

SIO multiplayer, the board as master: SC low for a transfer; the master's word on SD, UART-like at 115200 bps (start bit, 16
bits LSB first, stop bit); the master takes the game's SI low and the game answers on SD the same way. The board drives SC and
SD only low and otherwise lets go of them to the pull-ups, so it never fights a GBA that drives a line high. One packet per GBA
frame: the checksum transfer, then the eight words of a command 751 us apart (the game's master's timer).

## Telling the games apart

None of the existing tools does it on its own: mGBA reads the cartridge's game code (`AXV` Ruby, `AXP` Sapphire), and
GB-Link's web page has the user choose. The board sees only the link port. From pret's decompilations:

- FireRed/LeafGreen and Emerald look for the adapter when the player presses A on the main menu
  (`IsWirelessAdapterConnected`: the soft reset on SD, the ID exchange, then stop mode, 0x3D), and again each time the
  player talks to a Pokémon Center attendant. Without an adapter their attendants fall back to the cable club, so with the
  board always answering as an adapter they stay wireless.
- Ruby and Sapphire have no wireless code: they never pulse SD or clock the ID exchange. Their cable club opens a
  multiplayer link and takes SI for its role: low is the master, high the child. As a child it waits for the master's
  handshake (`0x8FFF`, answered `0xB9A0`).

So, from the moment the adapter goes on the wire:

1. The game is unknown, and SI is held high (the adapter mirrors SO onto SI only once the GBA clocks it: the ID exchange has
   no handshake, `librfu_sio32id.c`).
2. The first adapter word marks a wireless game (once the ID exchange is through, for the rest of the session).
3. While it is unknown, after 10 s and at most every 250 ms, with the port quiet and SC and SD high, and not within 5 s of
   an SD reset: one cable handshake transfer. A `0xB9A0` answer hands the port to the cable until the console goes off
   (every line low for 0.5 s) or the bridge stops the adapter; then the adapter is back, the game unknown again.

`LDN_CABLE_LOG` (in `ldn-console`'s defaults) prints which of the three it is, the cable's state and its last 16 log lines.

## Not done yet

- A Ruby or Sapphire that leaves its cable club stays on the cable side (handshaking) until the console goes off; a cartridge
  swapped while the board stays powered (USB) is found again only after that.
- No host side (RS leading a group), as in mGBA.
