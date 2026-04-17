/**
 * main.c - MCU ESP32-C5 bridge firmware entry point
 *
 * Boot sequence:
 *   1. NVS init
 *   2. Read hardware MAC → derive MCU ID (0-7) deterministically
 *   3. WiFi 6 station init (connect to host AP)
 *   4. Bridge init (UART + TCP client)
 *
 * MCU ID derivation — same binary on every board, no pre-flash config:
 *   The ESP32 has a factory-programmed 48-bit MAC address unique per chip.
 *   The first 3 bytes are the Espressif OUI (same for all boards); the last
 *   3 bytes are device-unique.  We FNV-1a hash the last 3 bytes and fold
 *   into [0, KWM_MAX_MCU) to get a stable, deterministic ID.
 *
 * Klipper config for this MCU (on the Pi), example for MCU ID 3:
 *   [mcu secondary]
 *   serial: /dev/kwm3      # PTY created by klipper_bridge.py
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "kwm_uart.h"
#include "wifi_sta.h"
#include "bridge.h"
#include "kwm_protocol.h"

static const char *TAG = "main";

/* ── MAC → MCU ID ────────────────────────────────────────────────────────── */

/**
 * Derive a stable MCU ID (0 .. KWM_MAX_MCU-1) from the hardware MAC.
 *
 * Uses FNV-1a over the device-unique octets (mac[3..5]) for good bit
 * distribution across the small ID space.  Same chip → same ID always.
 */
static uint8_t mac_to_mcu_id(const uint8_t mac[6]) {
    /* FNV-1a 32-bit: hash only the device-unique bytes (non-OUI part). */
    uint32_t h = 2166136261u;
    for (int i = 3; i < 6; i++)
        h = (h ^ mac[i]) * 16777619u;
    return (uint8_t)(h % KWM_MAX_MCU);
}

/* ── WiFi state callback ─────────────────────────────────────────────────── */

static void on_wifi_state(bool connected) {
    if (connected)
        ESP_LOGI(TAG, "WiFi connected to host AP");
    else
        ESP_LOGW(TAG, "WiFi disconnected – TCP client will reconnect");
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void) {
    /* Derive MCU ID from hardware MAC before doing anything else. */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint8_t mcu_id = mac_to_mcu_id(mac);

    ESP_LOGI(TAG, "=== Klipper WiFi MCU - MCU Bridge ESP32-C5 ===");
    ESP_LOGI(TAG, "IDF %s", IDF_VER);
    ESP_LOGI(TAG, "MAC  %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "MCU ID = %u  (hash of MAC[3:5] mod %d)",
             mcu_id, KWM_MAX_MCU);
    ESP_LOGI(TAG, "Pi PTY will appear as /dev/kwm%u", mcu_id);

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
    ESP_LOGI(TAG, "Starting bridge (mcu_id=%u)...", mcu_id);
    ESP_ERROR_CHECK(bridge_init(mcu_id, mac));

    ESP_LOGI(TAG, "MCU bridge firmware ready");
}
