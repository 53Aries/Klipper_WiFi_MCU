/**
 * main.c - Host ESP32-C5 firmware entry point
 *
 * Boot sequence:
 *   1. NVS init (required by WiFi driver)
 *   2. UART HAL init (Pi-facing transport)
 *   3. WiFi 6 soft-AP start
 *   4. Bridge init (TCP server + bridge tasks)
 *   5. Status task
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "kwm_uart.h"
#include "wifi_ap.h"
#include "bridge.h"
#include "led_status.h"
#include "kwm_protocol.h"

static const char *TAG = "main";

/* ── Status task ─────────────────────────────────────────────────────────── */

static void status_task(void *pvParam) {
    (void)pvParam;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Heap free: %lu bytes | UART rx pending: %d | WiFi STAs: %d",
                 (unsigned long)esp_get_free_heap_size(),
                 kwm_uart_rx_pending(),
                 wifi_ap_station_count());
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "=== Klipper WiFi MCU - Host ESP32-C5 ===");
    ESP_LOGI(TAG, "IDF %s | cores=%d", IDF_VER, chip.cores);
    ESP_LOGI(TAG, "TCP port %d | max MCUs %d", KWM_TCP_PORT, KWM_MAX_MCU);

    led_status_init();
    led_status_set(LED_STATE_BOOTING);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase and reinit");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ESP_LOGI(TAG, "Initialising UART transport...");
    ESP_ERROR_CHECK(kwm_uart_init());

    ESP_LOGI(TAG, "Starting WiFi 6 AP...");
    esp_err_t wifi_ret = wifi_ap_init();
    if (wifi_ret != ESP_OK) {
        led_status_set(LED_STATE_WIFI_ERROR);
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_ERROR_CHECK(wifi_ret);
    }
    led_status_set(LED_STATE_WIFI_UP);

    ESP_LOGI(TAG, "Starting bridge...");
    ESP_ERROR_CHECK(bridge_init());

    xTaskCreate(status_task, "status", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "Host firmware ready. Waiting for Pi UART and MCU connections.");
}
