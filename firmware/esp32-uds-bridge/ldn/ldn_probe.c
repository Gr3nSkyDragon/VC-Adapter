#include <ctype.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ldn_control.h"
#include "ldn_keys.h"
#include "pia_bridge.h"
#include "pia_crypto.h"
#include "esp_heap_caps.h"
#include "ldn_session.h"
#include "transport.h"
#include "ldn_udp.h"
#include "ldn_wire.h"
#define printf ldn_wire_printf
static esp_netif_t *s_station_netif;

#if !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32C6 && !CONFIG_IDF_TARGET_ESP32S3
#error "This firmware runs on the ESP32, ESP32-C3, ESP32-C6 and ESP32-S3"
#endif

#include "ldn_private_wifi.h"


#if ESP_IDF_VERSION_MAJOR != 6 || ESP_IDF_VERSION_MINOR != 1
#error "The private CCMP probe is ABI-pinned to ESP-IDF v6.1"
#endif


#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

static const char *TAG = "ldn_probe";
typedef struct {
    uint32_t promisc_action_count;
    uint32_t roc_action_count;
    uint32_t promisc_ldn_count;
    uint32_t roc_ldn_count;
    uint32_t eapol_dropped;
    uint8_t last_category;
    uint8_t last_source[6];
    uint8_t last_bssid[6];
} probe_stats_t;

static probe_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_probe_task;
static void remember_association(const uint8_t *frame, size_t length);

static struct {
    uint8_t body[1536];
    uint8_t source[6];
    uint8_t channel;
    size_t length;
} s_advertisement;

static void export_advertisement(void)
{
    static uint8_t body[1536];
    static char hex[3073];
    static const char digits[] = "0123456789abcdef";
    uint8_t source[6];
    portENTER_CRITICAL(&s_stats_lock);
    const size_t length = s_advertisement.length;
    const uint8_t channel = s_advertisement.channel;
    memcpy(body, s_advertisement.body, length);
    memcpy(source, s_advertisement.source, sizeof(source));
    s_advertisement.length = 0;
    portEXIT_CRITICAL(&s_stats_lock);
    if (length == 0) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        hex[2 * i] = digits[body[i] >> 4];
        hex[2 * i + 1] = digits[body[i] & 15];
    }
    hex[2 * length] = '\0';
    /* Not printed during a session, to save relay loop time. */
    const bool quiet = pia_bridge_in_session();
    if (!quiet) printf("LDN_ADV " MACSTR " %u %s\n", MAC2STR(source), channel, hex);
    /* Decoded on-device for the standalone bridge. */
    static ldn_network_t room;
    if (ldn_decode_advertisement(body, length, source, channel, &room)) {
        const char *host_name = "";
        for (int i = 0; i < room.member_count; ++i)
            if (room.members[i].index == 0) host_name = room.members[i].name;
        if (!quiet)
            printf("LDN_ROOM " MACSTR " ch=%d proto=%d comm=%016llx members=%d/%d host=%s app=%d\n",
                   MAC2STR(source), room.channel, room.protocol,
                   (unsigned long long)room.communication_id, room.member_count, room.maximum,
                   host_name, room.app_data_len);
        pia_bridge_room(&room);
    }
    fflush(stdout);
}

static void note_rssi(int8_t rssi);

static void remember_action(const uint8_t *header, const uint8_t *body,
                            size_t body_len, bool from_roc, uint8_t channel, int8_t rssi)
{
    if (header == NULL || body == NULL || body_len == 0) {
        return;
    }
    static const uint8_t broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t ldn_prefix[] = {0x7f, 0x00, 0x22, 0xaa, 0x04};
    const bool ldn = body_len >= sizeof(ldn_prefix) &&
                     memcmp(header + 4, broadcast, sizeof(broadcast)) == 0 &&
                     memcmp(body, ldn_prefix, sizeof(ldn_prefix)) == 0;
    if (ldn) note_rssi(rssi);
    portENTER_CRITICAL(&s_stats_lock);
    if (ldn && body_len <= sizeof(s_advertisement.body)) {
        memcpy(s_advertisement.body, body, body_len);
        memcpy(s_advertisement.source, header + 10, 6);
        s_advertisement.channel = channel;
        s_advertisement.length = body_len;
    }
    memcpy(s_stats.last_source, header + 10, sizeof(s_stats.last_source));
    memcpy(s_stats.last_bssid, header + 16, sizeof(s_stats.last_bssid));
    s_stats.last_category = body[0];
    if (from_roc) {
        ++s_stats.roc_action_count;
        s_stats.roc_ldn_count += ldn;
    } else {
        ++s_stats.promisc_action_count;
        s_stats.promisc_ldn_count += ldn;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

/* RSSI of LDN action frames since the last report. */
static int8_t s_last_ldn_rssi;
static int s_rssi_count, s_rssi_sum, s_rssi_min, s_rssi_max;

int8_t ldn_probe_last_rssi(void) { return s_last_ldn_rssi; }

void ldn_probe_rssi_window(int *count, int *average, int *low, int *high)
{
    *count = s_rssi_count;
    *average = s_rssi_count ? s_rssi_sum / s_rssi_count : 0;
    *low = s_rssi_count ? s_rssi_min : 0;
    *high = s_rssi_count ? s_rssi_max : 0;
    s_rssi_count = s_rssi_sum = 0;
    s_rssi_min = s_rssi_max = 0;
}

static void note_rssi(int8_t rssi)
{
    s_last_ldn_rssi = rssi;
    if (s_rssi_count == 0) { s_rssi_min = s_rssi_max = rssi; }
    else if (rssi < s_rssi_min) s_rssi_min = rssi;
    else if (rssi > s_rssi_max) s_rssi_max = rssi;
    s_rssi_sum += rssi;
    ++s_rssi_count;
}

static void promiscuous_rx(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (buffer == NULL) {
        return;
    }

    const wifi_promiscuous_pkt_t *packet = buffer;
    const uint8_t *frame = packet->payload;
    const size_t length = packet->rx_ctrl.sig_len;
    if (type == WIFI_PKT_DATA || type == WIFI_PKT_CTRL || type == WIFI_PKT_MGMT) {
        ldn_control_sniff(frame, length);
    }
    if (type != WIFI_PKT_MGMT) return;
    remember_association(frame, length);

    /* Management type 0, action subtype 13, little-endian frame control. */
    /* sig_len includes the 4-byte FCS. */
    if (length >= 29 && (frame[0] & 0xfcU) == 0xd0U) {
        remember_action(frame, frame + 24, length - 28, false, packet->rx_ctrl.channel, packet->rx_ctrl.rssi);
    }
}

static void enable_management_sniffer(void)
{
    const wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
            | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL
        ,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    const wifi_promiscuous_filter_t control = {.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ACK};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_ctrl_filter(&control));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
}


static int (*s_original_sta_connect)(uint8_t *bssid);
static uint8_t s_target_bssid[6];
static uint8_t s_ccmp_key[16];
static uint8_t s_station_mac[6];
static bool s_association_seen;
static char s_ssid[33];
static int64_t s_join_started;
static bool s_joining;
static atomic_int s_disconnect_reason = -1;

static void remember_association(const uint8_t *frame, size_t length)
{
    if (length < 34) {
        return;
    }
    const uint8_t subtype = frame[0] & 0xfcU;
    if ((subtype != 0x10 && subtype != 0x30) ||
        memcmp(frame + 4, s_station_mac, 6) != 0 ||
        memcmp(frame + 10, s_target_bssid, 6) != 0 ||
        memcmp(frame + 16, s_target_bssid, 6) != 0 ||
        frame[26] != 0 || frame[27] != 0) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    const bool notify = !s_association_seen;
    s_association_seen = true;
    portEXIT_CRITICAL(&s_stats_lock);
    if (notify && s_probe_task != NULL) {
        xTaskNotifyGive(s_probe_task);
    }
}

/* Exact RSN element emitted by kinnay/LDN: CCMP, PSK, capabilities 0x000c. */
static uint8_t s_ldn_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
    0x0c, 0x00,
};

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool parse_hex(const char *text, uint8_t *output, size_t output_len,
                      char separator)
{
    if (text == NULL || output == NULL || output_len == 0 ||
        output_len > SIZE_MAX / 3) {
        return false;
    }
    const size_t expected_len = output_len * 2 +
        (separator != '\0' ? output_len - 1 : 0);
    if (strlen(text) != expected_len) {
        return false;
    }
    for (size_t i = 0; i < output_len; ++i) {
        const int high = hex_nibble(*text++);
        const int low = hex_nibble(*text++);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
        if (separator != '\0' && i + 1 < output_len && *text++ != separator) {
            return false;
        }
    }
    return *text == '\0';
}

static int probe_sta_connect(uint8_t *bssid)
{
    /* Copy mode (last arg 0) takes raw IE bytes. Reference mode expects a 2-byte
       length prefix and overwrites it, corrupting a bare IE's tag/size. */
    int result = esp_wifi_set_appie_internal(
        LDN_WIFI_APPIE_RSN, s_ldn_rsn_ie, sizeof(s_ldn_rsn_ie), 0);
    if (result != 0) {
        ESP_LOGE(TAG, "pre-connect RSN IE install failed: %d", result);
        return result;
    }

    result = s_original_sta_connect != NULL ? s_original_sta_connect(bssid) : 0;
    if (result != 0) {
        return result;
    }

    /* The stock callback rebuilds the RSN IE; reinstall ours. */
    result = esp_wifi_set_appie_internal(
        LDN_WIFI_APPIE_RSN, s_ldn_rsn_ie, sizeof(s_ldn_rsn_ie), 0);
    if (result != 0) {
        ESP_LOGE(TAG, "post-connect RSN IE install failed: %d", result);
    }
    return result;
}

static void probe_sta_connected(uint8_t *bssid)
{
    if (bssid == NULL || memcmp(bssid, s_target_bssid, sizeof(s_target_bssid)) != 0) {
        ESP_LOGE(TAG, "unexpected association BSSID; ignoring callback");
        return;
    }
    ESP_LOGI(TAG, "WPA connected callback from " MACSTR,
             MAC2STR(s_target_bssid));
}

static int probe_rx_eapol(uint8_t *source, uint8_t *buffer, uint32_t length)
{
    (void)source;
    (void)buffer;
    (void)length;
    portENTER_CRITICAL(&s_stats_lock);
    ++s_stats.eapol_dropped;
    portEXIT_CRITICAL(&s_stats_lock);
    return 0;
}

static bool probe_in_4way_handshake(void)
{
    return false;
}

static void install_wpa_hook(void)
{
    ESP_ERROR_CHECK(wpa_cb == NULL ? ESP_ERR_INVALID_STATE : ESP_OK);
    struct ldn_wpa_funcs *probe = malloc(sizeof(*probe));
    ESP_ERROR_CHECK(probe == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    memcpy(probe, wpa_cb, sizeof(*probe));

    s_original_sta_connect = probe->wpa_sta_connect;
    probe->wpa_sta_connect = probe_sta_connect;
    probe->wpa_sta_connected_cb = probe_sta_connected;
    probe->wpa_sta_rx_eapol = probe_rx_eapol;
    probe->wpa_sta_in_4way_handshake = probe_in_4way_handshake;

    const int result = esp_wifi_register_wpa_cb_internal(probe);
    if (result != 0) {
        free(probe);
        ESP_ERROR_CHECK(result);
    }
    /* register_wpa_cb frees the old table; keep the supplicant owner in sync. */
    wpa_cb = probe;
    ESP_LOGW(TAG, "private ESP-IDF v6.1 WPA hook installed");
}

static bool read_key_back(int expected_index, enum ldn_key_flag flag,
                          const uint8_t *expected, int *driver_result)
{
    uint8_t interface = WIFI_IF_STA;
    int algorithm = LDN_WIFI_WPA_ALG_NONE;
    int index = expected_index;
    uint8_t address[6];
    uint8_t key[16];
    memcpy(address, s_target_bssid, sizeof(address));
    memset(key, 0, sizeof(key));

    const int result = esp_wifi_get_sta_key_internal(
        &interface, &algorithm, address, &index, key, sizeof(key), flag);
    const bool matches = result == 0 && algorithm == LDN_WIFI_WPA_ALG_CCMP &&
                         index == expected_index &&
                         memcmp(key, expected, sizeof(key)) == 0;
    ESP_LOGI(TAG, "read key index %d: result=%d alg=%d match=%s",
             expected_index, result, algorithm, matches ? "yes" : "no");
    *driver_result = result;
    return matches;
}

static void install_ldn_keys(void)
{
    /* The pinned blob copies eight RSC bytes even for a six-byte CCMP PN. */
    uint8_t sequence[8] = {0};
    const enum ldn_key_flag pairwise_flags =
        LDN_KEY_FLAG_PAIRWISE | LDN_KEY_FLAG_RX | LDN_KEY_FLAG_TX;
    const enum ldn_key_flag group_flags = LDN_KEY_FLAG_GROUP | LDN_KEY_FLAG_RX;

    const int pairwise = esp_wifi_set_sta_key_internal(
        LDN_WIFI_WPA_ALG_CCMP, s_target_bssid, 0, 1, sequence,
        sizeof(sequence), s_ccmp_key, sizeof(s_ccmp_key), pairwise_flags);
    const int group = esp_wifi_set_sta_key_internal(
        LDN_WIFI_WPA_ALG_CCMP, s_target_bssid, 1, 0, sequence,
        sizeof(sequence), s_ccmp_key, sizeof(s_ccmp_key), group_flags);

    ESP_LOGI(TAG, "key install: pairwise=%d group=%d", pairwise, group);
    /* Readback is diagnostic only. The pinned S3 driver reads back neither key (no pairwise
       getter, group getter omits the key). The encrypted LDN auth exchange verifies them. */
    int pairwise_read = 0, group_read = 0;
    const bool pairwise_ok = read_key_back(0, LDN_KEY_FLAG_PAIRWISE, s_ccmp_key, &pairwise_read);
    const bool group_ok = read_key_back(1, LDN_KEY_FLAG_GROUP, s_ccmp_key, &group_read);
    printf("LDN_KEY_STATUS pairwise_result=%d group_result=%d pairwise_readback=%u group_readback=%u readback_results=%d,%d\n",
           pairwise, group, pairwise_ok, group_ok, pairwise_read, group_read);
    if (pairwise != 0 || group != 0) {
        ESP_LOGE(TAG, "CCMP key install failed; not authorizing port");
        return;
    }

    const bool authorized = esp_wifi_auth_done_internal();
    printf("LDN_AUTH_PORT %u\n", authorized);
}

static void wifi_event(void *argument, esp_event_base_t base, int32_t id,
                       void *data)
{
    (void)argument;
    (void)base;
    if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = data;
        portENTER_CRITICAL(&s_stats_lock);
        const uint32_t eapol_dropped = s_stats.eapol_dropped;
        portEXIT_CRITICAL(&s_stats_lock);
        ESP_LOGI(TAG, "STA_CONNECTED aid=%u channel=%u, eapol dropped=%" PRIu32,
                 event->aid, event->channel, eapol_dropped);
        ldn_control_link(true);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "STA_DISCONNECTED reason=%u", event->reason);
        atomic_store(&s_disconnect_reason, event->reason);
        ldn_control_link(false);
    }
}

const char *ldn_session_ssid(void) { return s_ssid[0] ? s_ssid : "-"; }

void ldn_session_stop(void)
{
    s_joining = false;
    ldn_udp_stop();
    esp_wifi_disconnect();
    /* Stop the driver before replacing key material or accepting another room. */
    esp_wifi_stop();
    s_association_seen = false;
    memset(s_ccmp_key, 0, sizeof(s_ccmp_key));
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_target_bssid, 0, sizeof(s_target_bssid));
    ldn_control_target(s_target_bssid);
    esp_wifi_start();
    bridge_wifi_tx_power();
    esp_wifi_set_ps(WIFI_PS_NONE);
    enable_management_sniffer();
    portENTER_CRITICAL(&s_stats_lock);
    s_advertisement.length = 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

esp_err_t ldn_session_scan(unsigned channel)
{
    if (channel < 1 || channel > 11 || s_joining || s_ssid[0]) return ESP_ERR_INVALID_STATE;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

esp_err_t ldn_session_configure(const char *ssid, const char *bssid, const char *key, unsigned channel)
{
    uint8_t host[6], secret[16], ssid_bytes[16];
    if (channel < 1 || channel > 11 || !parse_hex(ssid, ssid_bytes, 16, '\0') ||
        !parse_hex(bssid, host, 6, ':') || (host[0] & 1) || !parse_hex(key, secret, 16, '\0'))
        return ESP_ERR_INVALID_ARG;
    ldn_session_stop();
    memcpy(s_ssid, ssid, 33);
    memcpy(s_target_bssid, host, 6); memcpy(s_ccmp_key, secret, 16);
    memset(secret, 0, sizeof(secret));
    /* Random station MAC per join, so a reconnect gets a fresh CCMP replay context. */
    esp_wifi_stop();
    esp_fill_random(s_station_mac, 6); s_station_mac[0] = (s_station_mac[0] & 0xfc) | 2;
    esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, s_station_mac);
    if (result != ESP_OK) return result;
    esp_wifi_start(); bridge_wifi_tx_power(); esp_wifi_set_ps(WIFI_PS_NONE); enable_management_sniffer();
    ldn_control_target(s_target_bssid);

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, s_ssid, 32);
    memcpy(config.sta.password, "00000000", 8);
    memcpy(config.sta.bssid, s_target_bssid, sizeof(s_target_bssid));
    config.sta.bssid_set = true;
    config.sta.channel = channel;
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = false;
    config.sta.pmf_cfg.required = false;

    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) return result;
    s_association_seen = false; s_joining = true; s_join_started = esp_timer_get_time();
    result = esp_wifi_connect();
    if (result != ESP_OK) s_joining = false;
    return result;
}

static void run_private_join(void)
{
    s_probe_task = xTaskGetCurrentTaskHandle();
#if CONFIG_FREERTOS_NUMBER_OF_CORES == 1
    /* Single core: the Pia tick (16.7 ms) must outrank the Pico UART pump (priority 10,
       44 ms of driver buffer). Below Wi-Fi and TCP/IP (23, 18); yields every pass. */
    vTaskPrioritySet(NULL, 11);
#endif
    ldn_control_init(s_station_netif, s_target_bssid);
    /* Claim the zstd context before a session starts; it needs one large contiguous block. */
    printf("LDN_PIA_PREPARE ok=%d size=%u heap=%u largest=%u psram=%u\n",
           pia_crypto_prepare(), (unsigned)pia_crypto_dctx_size(),
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    int64_t last_advertisement = 0;
    for (;;) {
        ldn_control_poll();
        pia_bridge_poll();
        int reason = atomic_exchange(&s_disconnect_reason, -1);
        if (reason >= 0) printf("LDN_DISCONNECTED %d\n", reason);
        const int64_t now = esp_timer_get_time();
        if (s_joining && s_association_seen && esp_wifi_sta_is_running_internal()) {
            s_joining = false; install_ldn_keys();
        } else if (s_joining && now - s_join_started > 15000000) {
            ldn_session_stop(); printf("LDN_ERROR ASSOCIATION_TIMEOUT\n");
        }
        if (now - last_advertisement >= 250000) { export_advertisement(); last_advertisement = now; }
        /* The GBA<->Switch protocol ticks every ~16.7 ms; keep this delay minimal. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Board hooks run before the radio starts (antenna switch, power rail). Weak no-op
   defaults. */
__attribute__((weak)) void bridge_board_init(void) {}
__attribute__((weak)) bool bridge_board_antenna(bool external) { (void)external; return false; }

void bridge_wifi_tx_power(void)
{
#if CONFIG_BRIDGE_WIFI_MAX_TX_POWER
    esp_wifi_set_max_tx_power(CONFIG_BRIDGE_WIFI_MAX_TX_POWER);
#endif
}

/* VC-Adapter merge: was app_main. The merged firmware's app_main (main/main.c) calls this when the board boots in
   Switch LDN mode. */
void ldn_app_main(void)
{
    bridge_board_init();
    /* Before anything else allocates: see pia_crypto_prepare. */
    pia_crypto_prepare();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_bytes));
    ESP_LOGI(TAG, "target=%s revision=%u cores=%u flash=%" PRIu32 " MiB IDF=%s",
             CONFIG_IDF_TARGET, chip.revision, chip.cores,
             flash_bytes / (1024 * 1024), esp_get_idf_version());
    ESP_LOGI(TAG, "console=%s", bridge_transport_name());
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* LDN assigns a static address after authentication; it does not use DHCP. */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.flags &= ~ESP_NETIF_DHCP_CLIENT;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    netif_config.base = &base;
    s_station_netif = esp_netif_new(&netif_config);
    ESP_ERROR_CHECK(s_station_netif == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(s_station_netif));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
    ESP_LOGI(TAG, "LDN static-address interface ready; automatic DHCP disabled");

    const wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if CONFIG_LDN_PROBE_LEGACY_PHY
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
#endif
    install_wpa_hook();
    ESP_ERROR_CHECK(esp_wifi_start());
    bridge_wifi_tx_power();
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    enable_management_sniffer();

    run_private_join();
}
