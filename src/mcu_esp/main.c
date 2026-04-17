/**
 * main.c - MCU ESP32-C5 bridge firmware entry point
 *
 * Boot sequence:
 *   1. NVS init
 *   2. WiFi 6 station init (connect to host AP)
 *   3. Bridge init (UART + TCP client)
 *
 * KWM_MCU_ID is set at compile time via -DKWM_MCU_ID=N in platformio.ini.
 * Default is 0. Set a unique ID per board.
 *
 * Klipper config for this MCU (on the Pi):
 *   [mcu secondary]
 *   serial: /dev/pts/1      # from klipper_bridge.py PTY
 *   # OR if you use the socket approach on the Pi directly:
 *   # serial: socket://192.168.42.1:8842
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "kwm_uart.h"
#include "wifi_sta.h"
#include "bridge.h"
#include "kwm_protocol.h"

static const char *TAG = "main";

#ifndef KWM_MCU_ID
#  define KWM_MCU_ID  0
#endif

/* ── WiFi state callback ─────────────────────────────────────────────────── */

static void on_wifi_state(bool connected) {
    if (connected)
        ESP_LOGI(TAG, "WiFi connected");
    else
        ESP_LOGW(TAG, "WiFi disconnected – TCP client will reconnect");
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "=== Klipper WiFi MCU - MCU Bridge ESP32-C5 ===");
    ESP_LOGI(TAG, "IDF %s | MCU ID=%d | baud=%d | uart_tx=%d | uart_rx=%d",
             IDF_VER, KWM_MCU_ID, KWM_UART_BAUD, KWM_UART_TX_PIN, KWM_UART_RX_PIN);

    /* NVS. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* WiFi 6 STA. */
    ESP_LOGI(TAG, "Connecting to WiFi AP '%s'...", KWM_WIFI_SSID);
    esp_err_t wifi_ret = wifi_sta_init(on_wifi_state);
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial WiFi connect failed; bridge will retry");
    }

    /* Bridge: UART + TCP. */
    ESP_LOGI(TAG, "Starting bridge (mcu_id=%d)...", KWM_MCU_ID);
    ESP_ERROR_CHECK(bridge_init(KWM_MCU_ID));

    ESP_LOGI(TAG, "MCU bridge firmware ready");
}
