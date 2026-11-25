/* Board setup before Wi-Fi starts (board.h).
 *
 * The Seeed XIAO ESP32-C6 part is ported from GB-Link Switch LDN's firmware/ESP32-C6/main/xiao_c6_board.c
 * (https://github.com/GB-Link/gb-link-switch-ldn, commit c972db4), which is AGPL-3.0 (../ldn/LICENSE); this file is under the same
 * licence. Alpha: built, not yet run on a C6.
 */
#include "board.h"

#include <stdbool.h>

#include "sdkconfig.h"

#if CONFIG_UDS_BOARD_XIAO_C6_RF_SWITCH
#include "driver/gpio.h"

/* The XIAO C6's antenna RF switch is off until GPIO3 is driven low. GPIO14 selects the antenna: low = onboard ceramic, high = U.FL
 * external. With both pins floating the radio only reaches strong nearby signals. */
#define RF_SWITCH_ENABLE_PIN 3 /* active low */
#define ANTENNA_SELECT_PIN 14  /* low: onboard, high: external U.FL */

void board_init(void)
{
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << RF_SWITCH_ENABLE_PIN) | (1ULL << ANTENNA_SELECT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pins);
    gpio_set_level(RF_SWITCH_ENABLE_PIN, 0);
    gpio_set_level(ANTENNA_SELECT_PIN, 0);
}

/* GB-Link Switch LDN's board hooks (ldn/ldn_probe.c has weak no-op ones): its own boot calls bridge_board_init again, which only
 * repeats the above, and its LDN_ANTENNA command picks the antenna. */
void bridge_board_init(void) { board_init(); }

bool bridge_board_antenna(bool external)
{
    gpio_set_level(ANTENNA_SELECT_PIN, external ? 1 : 0);
    return true;
}
#else
void board_init(void) {}
#endif
