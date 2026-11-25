#include "gba_vcc.h"

#include <stdbool.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "ldn_wire.h"
#define printf ldn_wire_printf

#if defined(CONFIG_BRIDGE_GBA_VCC_GPIO) && CONFIG_BRIDGE_GBA_VCC_GPIO >= 0
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define SAMPLE_US 250000
#define SAMPLES   8
#define SETTLE    3      /* readings in a row before a change is printed */
#define GB_MV     3800   /* pin 1 at or above: Game Boy mode (measured 4.5 V; GBA mode 3.16 V) */
#define GBA_MV    2000   /* at or above: 3.3 V, GBA mode; below: off */

static const char *const kStates[] = {"off", "gba", "gb"};

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static bool s_ready;
static int64_t s_next;
static int s_shown = -1, s_pending = -1, s_count;

/* Pin 1 in millivolts, or -1. */
static int read_pin1(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < SAMPLES; ++i)
    {
        int raw;
        if (adc_oneshot_read(s_unit, s_channel, &raw) == ESP_OK) { sum += raw; ++n; }
    }
    if (n == 0) return -1;
    int mv;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, sum / n, &mv) != ESP_OK)
        mv = sum / n * 3300 / 4095;   /* uncalibrated: close enough to tell 3.3 V from 5 V */
    return (int)((int64_t)mv * CONFIG_BRIDGE_GBA_VCC_DIVIDER / 1000);
}

void gba_vcc_start(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_BRIDGE_GBA_VCC_GPIO, &unit, &s_channel) != ESP_OK)
    {
        printf("LDN_GBA_VCC_ERROR GPIO %d has no ADC input\n", CONFIG_BRIDGE_GBA_VCC_GPIO);
        return;
    }
    const adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = unit};
    const adc_oneshot_chan_cfg_t channel_config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&unit_config, &s_unit) != ESP_OK ||
        adc_oneshot_config_channel(s_unit, s_channel, &channel_config) != ESP_OK)
    {
        printf("LDN_GBA_VCC_ERROR GPIO %d: ADC setup failed\n", CONFIG_BRIDGE_GBA_VCC_GPIO);
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = unit, .chan = s_channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cali = {
        .unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    s_ready = true;
}

void gba_vcc_poll(int64_t now_us)
{
    if (!s_ready || now_us < s_next) return;
    s_next = now_us + SAMPLE_US;
    const int mv = read_pin1();
    if (mv < 0) return;
    const int state = mv >= GB_MV ? 2 : mv >= GBA_MV ? 1 : 0;
    if (state == s_shown) { s_pending = -1; return; }
    if (state != s_pending) { s_pending = state; s_count = 0; }
    if (s_shown >= 0 && ++s_count < SETTLE) return;
    s_shown = state;
    s_pending = -1;
    printf("LDN_GBA_VCC %d %s\n", mv, kStates[state]);
}

#else

void gba_vcc_start(void) {}
void gba_vcc_poll(int64_t now_us) { (void)now_us; }

#endif
