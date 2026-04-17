/**
 * wifi_sta.c - WiFi 6 station mode for MCU ESP32-C5
 *
 * Connects to the hidden AP with WPA3-SAE.
 * Automatic reconnect on disassociation.
 */

#include "wifi_sta.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t  s_wifi_events;
static wifi_sta_state_cb_t s_state_cb;
static bool                s_connected;
static int                 s_retry_count;

/* ── Event handler ───────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_connected = false;
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            if (s_state_cb) s_state_cb(false);
            s_retry_count++;
            /* Always reconnect — Klipper needs the link permanently up.
             * The WiFi driver handles its own retry backoff internally. */
            ESP_LOGI(TAG, "Disconnected, reconnecting (attempt %d)...", s_retry_count);
            esp_wifi_connect();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        s_connected   = true;
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_state_cb) s_state_cb(true);
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t wifi_sta_init(wifi_sta_state_cb_t state_cb) {
    s_state_cb   = state_cb;
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,  wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e        = WPA3_SAE_PWE_BOTH,
            .pmf_cfg            = { .capable = true, .required = true },
        },
    };
    strncpy((char *)wifi_config.sta.ssid,     KWM_WIFI_SSID,     sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password,  KWM_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Enable WiFi 6 on STA interface. */
    esp_err_t ret = esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                        WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "WiFi 6 protocol set failed: %s (continuing)", esp_err_to_name(ret));

    /* Disable power save: Klipper requires consistent low-latency delivery.
     * WIFI_PS_MIN_MODEM (default) can add up to beacon_interval (100 ms)
     * of receive latency per packet. WIFI_PS_NONE removes that entirely. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait for first connection result. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP '%s'", KWM_WIFI_SSID);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Initial WiFi connection failed");
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_connected(void) {
    return s_connected;
}

esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms) {
    if (s_connected) return ESP_OK;
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                     ? portMAX_DELAY
                     : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, ticks);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
