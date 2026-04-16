/*
 * esp-usb-ap — Pi-side ESP32-C5 firmware
 *
 * Presents to the Pi as a USB CDC-NCM ethernet adapter (usb0).
 * Runs a WiFi 6 (802.11ax) softAP for MCU-side C5 boards.
 *
 * Data path:
 *   Pi <── USB CDC-NCM (TinyUSB) ──> C5 <── WiFi 6 AP (OFDMA) ──> MCU C5s
 *
 * TODO:
 *   - USB TinyUSB CDC-NCM init (tinyusb_net component)
 *   - WiFi softAP init (802.11ax, 5GHz, hidden SSID, WPA2)
 *   - Layer 2 bridge: forward Ethernet frames between USB and WiFi
 *   - DHCP server for MCU clients (192.168.42.x)
 *   - Static IP on usb0 side (192.168.42.1)
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb-ap";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "esp-usb-ap starting — stub only, implementation pending");

    /* TODO: usb_ap_init() */
    /* TODO: wifi_ap_init() */
    /* TODO: bridge_init()  */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "running...");
    }
}
