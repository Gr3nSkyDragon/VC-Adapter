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
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* SI and SO are named from this board's side, as on the GB-Link. */
#define PIN_SC CONFIG_UDS_LINK_PORT_SC_GPIO
#define PIN_SO CONFIG_UDS_LINK_PORT_SO_GPIO /* this board's serial out: to the console's SI */
#define PIN_SI CONFIG_UDS_LINK_PORT_SI_GPIO /* this board's serial in: from the console's SO */
#define PIN_SD CONFIG_UDS_LINK_PORT_SD_GPIO /* unused by a Game Boy's link: held as a plain input */
#define PIN_SENSE CONFIG_UDS_LINK_PORT_SENSE_GPIO
#define CLOCK_HZ CONFIG_UDS_LINK_PORT_CLOCK_HZ

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

/* How long linkport_clock_busy watches: a little more than one bit of a Game Boy's own 8192 Hz clock, so that a byte the other side
 * clocks is seen whichever half of a bit it is in. */
#define BUSY_WATCH_US 125
#define BUSY_SAMPLE_US 4

/* The sense pin reads the port's VCC through two equal resistors. Below OFF there is nothing; up to GB it is a GBA game's 3.3 V. */
#define SENSE_DIVIDER 2
#define SENSE_OFF_MV 1500
#define SENSE_GB_MV 4100

static bool s_enabled;
static uint32_t s_half_cycles; /* half a clock period, in CPU cycles */
static portMUX_TYPE s_byte_lock = portMUX_INITIALIZER_UNLOCKED;
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
    s_half_cycles = (uint32_t)((uint64_t)esp_rom_get_cpu_ticks_per_us() * 1000000u / (2u * CLOCK_HZ));
    if (!s_half_cycles) s_half_cycles = 1;
    /* Open drain, released (high) from the start and between bytes: the clock and this board's serial out. */
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

/* Spins until `cycles` after *mark, and moves *mark on: the edges keep their spacing whatever the GPIO calls cost. */
static inline void wait_from(uint32_t *mark, uint32_t cycles)
{
    *mark += cycles;
    while ((int32_t)((uint32_t)esp_cpu_get_cycle_count() - *mark) < 0) {
    }
}

uint8_t linkport_exchange(uint8_t out)
{
    /* The byte goes out as the GB-Link's Pico sends one: fast, push-pull, in one piece. At 8192 Hz a byte took a millisecond, and a
     * cartridge that re-armed its serial port in the middle of one (Pokemon re-arms in its serial interrupt, which waits behind the
     * frame's other work) shifted only its tail and lost its place in every byte after: the first hardware run read the cartridge's
     * FD padding rotated by a few bits (EF EF EF..., BF BF BF...) and the cartridge read ours the same way. A byte of a few tens of
     * microseconds is either missed whole or taken whole. Interrupts are held off for it so that nothing stretches it, and the
     * lines are driven both ways for crisp edges through the level shifter; between bytes they go back to open drain. */
    gpio_set_direction(PIN_SC, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(PIN_SO, GPIO_MODE_INPUT_OUTPUT);
    uint8_t in = 0;
    portENTER_CRITICAL(&s_byte_lock);
    uint32_t mark = (uint32_t)esp_cpu_get_cycle_count();
    for (int bit = 7; bit >= 0; --bit) {
        /* Falling edge: both sides put their bit out. Each reads the other's at the end of the low half, before the rising edge. */
        gpio_set_level(PIN_SC, 0);
        gpio_set_level(PIN_SO, (out >> bit) & 1);
        wait_from(&mark, s_half_cycles);
        in = (uint8_t)((in << 1) | (gpio_get_level(PIN_SI) & 1));
        gpio_set_level(PIN_SC, 1);
        wait_from(&mark, s_half_cycles);
    }
    gpio_set_level(PIN_SO, 1);
    portEXIT_CRITICAL(&s_byte_lock);
    gpio_set_direction(PIN_SC, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(PIN_SO, GPIO_MODE_INPUT_OUTPUT_OD);
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
