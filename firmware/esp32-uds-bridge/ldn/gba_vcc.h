#pragma once
/* Link port pin 1: 3.3 V while the GBA runs a GBA game, 5 V with a Game Boy or Game Boy
   Color cartridge. With CONFIG_BRIDGE_GBA_VCC_GPIO wired to pin 1 through a divider, the
   bridge prints LDN_GBA_VCC at boot and whenever pin 1 moves between off, 3.3 V and 5 V. */
#include <stdint.h>

void gba_vcc_start(void);
void gba_vcc_poll(int64_t now_us);
