#include "wifi.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"

#define TAG "wifi"
#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected — reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    // Static IP — no DHCP wait on reconnect
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 42, 11) },  // STATIC_IP
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 42,  1) },  // STATIC_GATEWAY
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255,  0) },
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               event_handler, NULL));

    uint8_t bssid[6] = AP_BSSID;
    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid           = WIFI_SSID,
            .password       = WIFI_PASSWORD,
            .channel        = WIFI_CHANNEL,
            .bssid_set      = AP_BSSID_SET,
            .scan_method    = WIFI_FAST_SCAN,
            .pmf_cfg        = { .capable = true, .required = false },
        },
    };
    memcpy(wifi_cfg.sta.bssid, bssid, 6);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    // No radio sleep — critical for latency
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to %s ch%d...", WIFI_SSID, WIFI_CHANNEL);
}

void wifi_wait_connected(void)
{
    xEventGroupWaitBits(s_wifi_events, CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}
