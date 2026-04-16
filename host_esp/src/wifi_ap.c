/**
 * wifi_ap.c - WiFi 6 soft-AP for host ESP32-C5
 *
 * Creates a hidden 802.11ax AP. The SSID and password are taken from
 * KWM_WIFI_SSID / KWM_WIFI_PASSWORD in kwm_protocol.h; override via
 * sdkconfig.defaults (CONFIG_KWM_WIFI_SSID / CONFIG_KWM_WIFI_PASSWORD).
 *
 * Static IP is assigned to the AP interface (KWM_AP_IP). MCU ESPs receive
 * DHCP addresses in the 192.168.42.0/24 range.
 */

#include "wifi_ap.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "kwm_protocol.h"

static const char *TAG = "wifi_ap";

/* Reachable config: allow sdkconfig overrides at build time. */
#ifdef CONFIG_KWM_WIFI_SSID
#  define AP_SSID     CONFIG_KWM_WIFI_SSID
#else
#  define AP_SSID     KWM_WIFI_SSID
#endif

#ifdef CONFIG_KWM_WIFI_PASSWORD
#  define AP_PASS     CONFIG_KWM_WIFI_PASSWORD
#else
#  define AP_PASS     KWM_WIFI_PASSWORD
#endif

static int s_station_count;

/* ── Event handler ───────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base != WIFI_EVENT) return;

    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        s_station_count++;
        ESP_LOGI(TAG, "STA connected: " MACSTR " (AID %d), total=%d",
                 MAC2STR(e->mac), e->aid, s_station_count);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *e = data;
        s_station_count--;
        if (s_station_count < 0) s_station_count = 0;
        ESP_LOGI(TAG, "STA disconnected: " MACSTR " (AID %d), total=%d",
                 MAC2STR(e->mac), e->aid, s_station_count);
        break;
    }
    default:
        break;
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t wifi_ap_init(void) {
    esp_err_t ret;

    /* Initialise TCP/IP stack and default event loop (idempotent). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create the default AP netif. */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    /* Assign static IP to the AP interface. */
    esp_netif_dhcps_stop(ap_netif);
    {
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ip4addr_aton(KWM_AP_IP,      (ip4_addr_t *)&ip_info.ip);
        ip4addr_aton(KWM_AP_IP,      (ip4_addr_t *)&ip_info.gw);
        ip4addr_aton(KWM_AP_NETMASK, (ip4_addr_t *)&ip_info.netmask);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    }
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    /* WiFi init. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register event handler. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    /* AP configuration. */
    wifi_config_t wifi_config = {
        .ap = {
            .ssid            = AP_SSID,
            .ssid_len        = strlen(AP_SSID),
            .password        = AP_PASS,
            .channel         = KWM_WIFI_CHANNEL,
            .authmode        = WIFI_AUTH_WPA3_PSK,
            .ssid_hidden     = 1,              /* hidden SSID              */
            .max_connection  = KWM_WIFI_MAX_STA,
            .beacon_interval = 100,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    /* Copy SSID explicitly (it's a fixed array). */
    strncpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    /* Enable WiFi 6 (802.11ax) on the AP interface. */
    ret = esp_wifi_set_protocol(WIFI_IF_AP,
              WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
              WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    if (ret != ESP_OK) {
        /* Non-fatal: log and continue. Older SDK builds may not support this. */
        ESP_LOGW(TAG, "WiFi 6 protocol set failed (%s), continuing with default",
                 esp_err_to_name(ret));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi 6 AP started: SSID=%s (hidden) channel=%d IP=%s",
             AP_SSID, KWM_WIFI_CHANNEL, KWM_AP_IP);

    return ESP_OK;
}

int wifi_ap_station_count(void) {
    return s_station_count;
}
