/**
 * main.c - Host ESP32-C5 firmware entry point
 *
 * Boot sequence:
 *   1. NVS init (required by WiFi driver)
 *   2. SPI slave HAL init
 *   3. WiFi 6 soft-AP start
 *   4. Bridge init (TCP server + bridge tasks)
 *   5. Optional status task printing periodic stats
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "kwm_spi.h"
#include "wifi_ap.h"
#include "bridge.h"
#include "kwm_protocol.h"

static const char *TAG = "main";

/* ── Status task ─────────────────────────────────────────────────────────── */

static void status_task(void *pvParam) {
    (void)pvParam;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));  /* log every 10 s */
        ESP_LOGI(TAG, "Heap free: %lu bytes | SPI rx pending: %d | WiFi STAs: %d",
                 (unsigned long)esp_get_free_heap_size(),
                 kwm_spi_rx_pending(),
                 wifi_ap_station_count());
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void) {
    /* Print build info. */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "=== Klipper WiFi MCU - Host ESP32-C5 ===");
    ESP_LOGI(TAG, "IDF %s | cores=%d | flash=%dMB",
             IDF_VER, chip.cores,
             (int)(2 << (chip.revision & 0x0F)));
    ESP_LOGI(TAG, "Protocol: SPI frame %d bytes | TCP port %d | max MCUs %d",
             KWM_SPI_FRAME_LEN, KWM_TCP_PORT, KWM_MAX_MCU);

    /* NVS – required by WiFi. Erase if partition has been upgraded. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase and reinit");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* SPI slave. */
    ESP_LOGI(TAG, "Initialising SPI slave...");
    ESP_ERROR_CHECK(kwm_spi_init());

    /* WiFi 6 AP. */
    ESP_LOGI(TAG, "Starting WiFi 6 AP...");
    ESP_ERROR_CHECK(wifi_ap_init());

    /* Bridge (TCP server + bridge tasks). */
    ESP_LOGI(TAG, "Starting bridge...");
    ESP_ERROR_CHECK(bridge_init());

    /* Status task (low priority). */
    xTaskCreate(status_task, "status", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "Host firmware ready. Waiting for Pi SPI master and MCU connections.");
}
