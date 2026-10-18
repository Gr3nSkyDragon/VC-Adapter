/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * Diagnostics for the 3DS side (diag.h).
 */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "diag";

static diag_queue_t s_queue;
static size_t s_reclaimed;
static esp_err_t s_reclaim_result = ESP_OK;

/* ESP-IDF's log, one message at a time. Called from whichever task logs (never from an interrupt: ESP_LOGx is not allowed there), so
 * the line is built on that task's stack and only queued. */
static int log_vprintf(const char *format, va_list args)
{
    char line[200];
    const int written = vsnprintf(line, sizeof(line), format, args);
    if (written <= 0 || !s_queue) return written;
    size_t length = (size_t)written < sizeof(line) ? (size_t)written : sizeof(line) - 1;
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
    if (length) s_queue(line, length);
    return written;
}

void diag_start(diag_queue_t queue)
{
    s_queue = queue;
    esp_log_set_vprintf(log_vprintf);
    esp_log_level_set("*", ESP_LOG_INFO);
}

void diag_set_reclaimed(size_t bytes, esp_err_t result)
{
    s_reclaimed = bytes;
    s_reclaim_result = result;
}

static const char *reset_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
    }
}

void diag_report(const char *when)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    ESP_LOGW(TAG,
             "%s: last reset %s; Switch memory reclaimed %u bytes (%s); internal free %u, largest %u, lowest ever %u; DMA free %u, "
             "largest %u",
             when, reset_text(esp_reset_reason()), (unsigned)s_reclaimed, esp_err_to_name(s_reclaim_result),
             (unsigned)heap_caps_get_free_size(internal), (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_minimum_free_size(internal), (unsigned)heap_caps_get_free_size(dma),
             (unsigned)heap_caps_get_largest_free_block(dma));
}
