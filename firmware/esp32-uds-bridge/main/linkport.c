/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * A Game Boy link port on this board's GPIOs (see linkport.h).
 */
#include "linkport.h"

#include "sdkconfig.h"

#if CONFIG_UDS_LINK_PORT

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

/* SI and SO are named from this board's side, as on the GB-Link. */
#define PIN_SC CONFIG_UDS_LINK_PORT_SC_GPIO
#define PIN_SO CONFIG_UDS_LINK_PORT_SO_GPIO /* this board's serial out: to the console's SI */
#define PIN_SI CONFIG_UDS_LINK_PORT_SI_GPIO /* this board's serial in: from the console's SO */
#define PIN_SD CONFIG_UDS_LINK_PORT_SD_GPIO /* unused by a Game Boy's link: held as a plain input */
#define PIN_SENSE CONFIG_UDS_LINK_PORT_SENSE_GPIO
#define HALF_PERIOD_US CONFIG_UDS_LINK_PORT_HALF_PERIOD_US

/* Two jobs on one GPIO would fight over it: menuconfig has to keep them apart. */
#define PIN_IS_LINE(p) ((p) == PIN_SC || (p) == PIN_SO || (p) == PIN_SI)
#if PIN_SC == PIN_SO || PIN_SC == PIN_SI || PIN_SO == PIN_SI
#error "CONFIG_UDS_LINK_PORT: SC, SI and SO need three different GPIOs"
#endif
#if PIN_SD >= 0 && PIN_IS_LINE(PIN_SD)
#error "CONFIG_UDS_LINK_PORT: SD shares a GPIO with SC, SI or SO"
#endif
#if PIN_SENSE >= 0 && (PIN_IS_LINE(PIN_SENSE) || PIN_SENSE == PIN_SD)
#error "CONFIG_UDS_LINK_PORT: the sense pin shares a GPIO with SC, SI, SO or SD"
#endif
#if CONFIG_UDS_PICO_LINK
#define PIN_IS_PICO(p) ((p) >= 0 && ((p) == CONFIG_UDS_PICO_LINK_TX_GPIO || (p) == CONFIG_UDS_PICO_LINK_RX_GPIO))
#if PIN_IS_PICO(PIN_SC) || PIN_IS_PICO(PIN_SO) || PIN_IS_PICO(PIN_SI) || PIN_IS_PICO(PIN_SD) || PIN_IS_PICO(PIN_SENSE)
#error "CONFIG_UDS_PICO_LINK and CONFIG_UDS_LINK_PORT share a GPIO (both default to GPIO1 and GPIO2): move one of them in menuconfig"
#endif
#endif

/* How long linkport_clock_busy watches: a little more than one bit of a Game Boy's own 8192 Hz clock, so that a byte the other side
 * clocks is seen whichever half of a bit it is in. */
#define BUSY_WATCH_US 125
#define BUSY_SAMPLE_US 4

/* The sense pin reads the port's VCC through two equal resistors. Below OFF there is nothing; up to GB it is a GBA game's 3.3 V. */
#define SENSE_DIVIDER 2
#define SENSE_OFF_MV 1500
#define SENSE_GB_MV 4100

static bool s_enabled;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_sense_channel;
static bool s_sense_ok;

static void sense_init(void)
{
    if (PIN_SENSE < 0) return;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_SENSE, &unit, &s_sense_channel) != ESP_OK || unit != ADC_UNIT_1) return; /* ADC2 is Wi-Fi's */
    const adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit_config, &s_adc) != ESP_OK) return;
    const adc_oneshot_chan_cfg_t channel_config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(s_adc, s_sense_channel, &channel_config) != ESP_OK) return;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1, .chan = s_sense_channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    s_sense_ok = true;
}

esp_err_t linkport_init(void)
{
    /* Open drain, released (high) from the start: the clock and this board's serial out. */
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_SC) | (1ULL << PIN_SO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << PIN_SI,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* nobody there reads as 1s, as on a real cable */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(PIN_SC, 1);
    gpio_set_level(PIN_SO, 1);
    esp_err_t err = gpio_config(&outputs);
    if (err == ESP_OK) err = gpio_config(&input);
#if PIN_SD >= 0
    if (err == ESP_OK) {
        /* SD: an input with no pull, so this board never drives it or tips it either way. */
        const gpio_config_t sd = {
            .pin_bit_mask = 1ULL << PIN_SD,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&sd);
    }
#endif
    if (err != ESP_OK) return err;
    sense_init();
    s_enabled = true;
    return ESP_OK;
}

bool linkport_enabled(void)
{
    return s_enabled;
}

enum linkport_power linkport_power(uint32_t *millivolts)
{
    if (millivolts) *millivolts = 0;
    if (!s_sense_ok) return LINKPORT_POWER_UNKNOWN;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, s_sense_channel, &raw) != ESP_OK) return LINKPORT_POWER_UNKNOWN;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095; /* uncalibrated: roughly */
    const uint32_t port_mv = (uint32_t)mv * SENSE_DIVIDER;
    if (millivolts) *millivolts = port_mv;
    if (port_mv < SENSE_OFF_MV) return LINKPORT_POWER_OFF;
    return port_mv < SENSE_GB_MV ? LINKPORT_POWER_GBA : LINKPORT_POWER_GB;
}

bool linkport_clock_busy(void)
{
    const int64_t end = esp_timer_get_time() + BUSY_WATCH_US;
    do {
        if (!gpio_get_level(PIN_SC)) return true;
        esp_rom_delay_us(BUSY_SAMPLE_US);
    } while (esp_timer_get_time() < end);
    return false;
}

uint8_t linkport_exchange(uint8_t out)
{
    uint8_t in = 0;
    for (int bit = 7; bit >= 0; --bit) {
        /* Falling edge: both sides put their bit out. Each reads the other's before the rising edge. Being interrupted only stretches
         * the clock, which a slave does not mind: it moves on edges, not on time. */
        gpio_set_level(PIN_SC, 0);
        gpio_set_level(PIN_SO, (out >> bit) & 1);
        esp_rom_delay_us(HALF_PERIOD_US);
        in = (uint8_t)((in << 1) | (gpio_get_level(PIN_SI) & 1));
        gpio_set_level(PIN_SC, 1);
        esp_rom_delay_us(HALF_PERIOD_US);
    }
    gpio_set_level(PIN_SO, 1);
    return in;
}

void linkport_release(void)
{
    if (!s_enabled) return;
    gpio_set_level(PIN_SC, 1);
    gpio_set_level(PIN_SO, 1);
}

#else /* !CONFIG_UDS_LINK_PORT */

esp_err_t linkport_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool linkport_enabled(void)
{
    return false;
}

enum linkport_power linkport_power(uint32_t *millivolts)
{
    if (millivolts) *millivolts = 0;
    return LINKPORT_POWER_UNKNOWN;
}

bool linkport_clock_busy(void)
{
    return false;
}

uint8_t linkport_exchange(uint8_t out)
{
    (void)out;
    return 0xFF;
}

void linkport_release(void)
{
}

#endif
