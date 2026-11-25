/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "port_watch.h"

#include "sdkconfig.h"

#if CONFIG_UDS_LINK_PORT

#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linkport.h"

#if UDS_HAVE_LDN && CONFIG_UDS_RFU_ADAPTER
#include "rfu_adapter.h"
#endif

#define LINES_DEAD_US 500000        /* every line low this long: the console is off */
#define HOST_QUIET_US 5000000       /* a PC that has said nothing this long has let go of the board */
#define MODE_STEADY_US 1000000      /* the other mode's reading, held this long, switches */
#define MODE_SETTLED_US 30000000    /* a mode run this long without wanting to switch clears the switch count */
#define MODE_SWITCHES_IN_A_ROW 4    /* then the reading is taken as unreliable until the next power-up */
#define SWITCH_MAGIC 0x706F7274u

static bridge_mode_t s_mode;
static int (*s_print)(const char *format, ...);
static volatile int64_t s_host_us;   /* the last byte from a PC (0: none since boot) */
static int64_t s_low_since_us;       /* every line low since (0: one is high) */
static int64_t s_off_since_us;       /* the console off (and no PC) since, for sleep */
static int64_t s_want_since_us;      /* the other mode's reading since */
static int s_last_power = -1;
static bool s_told_host, s_told_limit;

/* Mode switches made on the sense pin's word, in a row: kept across those restarts, not across power-up. */
static RTC_NOINIT_ATTR uint32_t s_switch_magic;
static RTC_NOINIT_ATTR uint32_t s_switches;

#define LOG(...) do { if (s_print) s_print(__VA_ARGS__); } while (0)

static const char *mode_name(bridge_mode_t mode)
{
    return mode == BRIDGE_MODE_UDS ? "3DS mode" : "Switch mode";
}

static const char *power_name(enum linkport_power power)
{
    switch (power) {
    case LINKPORT_POWER_OFF: return "off";
    case LINKPORT_POWER_GBA: return "a GBA game (3.3 V), or nothing";
    case LINKPORT_POWER_GB: return "a Game Boy game (5 V)";
    case LINKPORT_POWER_UNKNOWN:
    default: return "unknown (no sense pin)";
    }
}

void port_watch_start(bridge_mode_t mode, int (*print)(const char *format, ...))
{
    s_mode = mode;
    s_print = print;
    if (s_switch_magic != SWITCH_MAGIC || esp_reset_reason() != ESP_RST_SW) s_switches = 0;
    s_switch_magic = SWITCH_MAGIC;
    linkport_sense_start();
    LOG("PORT_SENSE %s, booted in %s%s\n", linkport_sense_how(), mode_name(mode),
        s_switches ? " by the sense pin" : "");
}

void port_watch_note_host(void)
{
    const int64_t now = esp_timer_get_time();
    s_host_us = now ? now : 1;
}

bool port_watch_host_quiet(void)
{
    const int64_t host = s_host_us;
    return !host || esp_timer_get_time() - host >= HOST_QUIET_US;
}

#if CONFIG_UDS_LINK_PORT_SLEEP_S > 0
/* The lines that rise when the console comes on (the shifter's pull-ups to the port's VCC): any of them wakes the board. In deep sleep
 * the pins are let go (no drive, no pull), so nothing on this side feeds the switched-off console. */
static void sleep_now(void)
{
    LOG("PORT_SENSE the console is off and no PC is there: sleeping until a link line goes high\n");
#if UDS_HAVE_LDN && CONFIG_UDS_RFU_ADAPTER
    rfu_adapter_stop();
#endif
    vTaskDelay(pdMS_TO_TICKS(20)); /* the adapter's core leaves its link loop and lets interrupts in; the line above goes out */
    const int pins[] = {CONFIG_UDS_LINK_PORT_SC_GPIO, CONFIG_UDS_LINK_PORT_SI_GPIO, CONFIG_UDS_LINK_PORT_SD_GPIO};
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] < 0 || !rtc_gpio_is_valid_gpio(pins[i])) continue;
        rtc_gpio_pullup_dis(pins[i]);
        rtc_gpio_pulldown_dis(pins[i]);
        mask |= 1ULL << pins[i];
    }
    if (!mask || esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_HIGH) != ESP_OK) return; /* nothing could wake it */
    esp_deep_sleep_start();
}
#endif

static void sleep_poll(bool console_off, int64_t now_us)
{
#if CONFIG_UDS_LINK_PORT_SLEEP_S > 0
    if (!console_off) {
        s_off_since_us = 0;
        return;
    }
    if (!s_off_since_us) s_off_since_us = now_us ? now_us : 1;
    if (now_us - s_off_since_us < (int64_t)CONFIG_UDS_LINK_PORT_SLEEP_S * 1000000) return;
    if (usb_serial_jtag_is_connected()) {
        s_off_since_us = now_us; /* a PC is there: look again later */
        return;
    }
    sleep_now();
#else
    (void)console_off;
    (void)now_us;
#endif
}

static void mode_poll(bool lines_up, int64_t now_us)
{
#if CONFIG_UDS_LINK_PORT_AUTO_MODE
    if (!bridge_mode_has_ldn()) return;
    const enum linkport_power power = linkport_power(NULL);
    if ((int)power != s_last_power) {
        s_last_power = (int)power;
        LOG("PORT_SENSE pin 1: %s\n", power_name(power));
    }
    /* A Game Boy's 5 V means 3DS mode. A low reading means a GBA game only while the console's lines are up: read as high/low, it is
     * also what a console switched off gives. */
    bridge_mode_t want = s_mode;
    if (power == LINKPORT_POWER_GB) want = BRIDGE_MODE_UDS;
    else if (power == LINKPORT_POWER_GBA && lines_up) want = BRIDGE_MODE_LDN;
    if (want == s_mode) {
        s_want_since_us = 0;
        s_told_host = false;
        if (s_switches && now_us >= MODE_SETTLED_US) s_switches = 0;
        return;
    }
    if (!port_watch_host_quiet()) {
        s_want_since_us = 0;
        if (!s_told_host) LOG("PORT_SENSE pin 1 says %s, but a PC is talking to the board: it picks the mode\n", mode_name(want));
        s_told_host = true;
        return;
    }
    if (s_switches >= MODE_SWITCHES_IN_A_ROW) {
        if (!s_told_limit) LOG("PORT_SENSE pin 1 changed the mode %d times in a row: not switching again until power-up\n", (int)s_switches);
        s_told_limit = true;
        return;
    }
    if (!s_want_since_us) {
        s_want_since_us = now_us ? now_us : 1;
        return;
    }
    if (now_us - s_want_since_us < MODE_STEADY_US) return;
    ++s_switches;
    LOG("PORT_SENSE pin 1: %s: switching to %s\n", power_name(power), mode_name(want));
    vTaskDelay(pdMS_TO_TICKS(20));
    bridge_mode_switch(want);
#else
    (void)lines_up;
    (void)now_us;
#endif
}

void port_watch_poll(bool lines_low, int64_t now_us)
{
    if (!lines_low) s_low_since_us = 0;
    else if (!s_low_since_us) s_low_since_us = now_us ? now_us : 1;
    const bool console_off = s_low_since_us && now_us - s_low_since_us >= LINES_DEAD_US;
    sleep_poll(console_off, now_us);
    mode_poll(!lines_low, now_us);
}

#if UDS_HAVE_LDN && CONFIG_UDS_RFU_ADAPTER
/* Switch mode: the emulated adapter has the link pins; it says when they have all been low a while. Before the adapter is on the wire
 * that is never, which keeps the board awake and leaves a low reading alone (a high one still means a Game Boy). */
static void switch_side_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        port_watch_poll(rfu_adapter_lines_dead(), esp_timer_get_time());
    }
}
#endif

void port_watch_start_switch_side(void)
{
#if UDS_HAVE_LDN && CONFIG_UDS_RFU_ADAPTER
    xTaskCreatePinnedToCore(switch_side_task, "port_watch", 3072, NULL, 2, NULL, 0);
#endif
}

#else /* !CONFIG_UDS_LINK_PORT */

void port_watch_start(bridge_mode_t mode, int (*print)(const char *format, ...))
{
    (void)mode;
    (void)print;
}

void port_watch_note_host(void)
{
}

bool port_watch_host_quiet(void)
{
    return true;
}

void port_watch_poll(bool lines_low, int64_t now_us)
{
    (void)lines_low;
    (void)now_us;
}

void port_watch_start_switch_side(void)
{
}

#endif
