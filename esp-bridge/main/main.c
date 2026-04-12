#include "wifi.h"
#include "bridge.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"

#define TAG "main"

// Watchdog timeout — if bridge_task hangs for this long, C5 resets itself.
// Must be longer than the worst expected WiFi reconnect time (~5s) to avoid
// false triggers during association. 30s is a safe conservative value.
#define WDT_TIMEOUT_S 30

static void stm32_gpio_init(void)
{
    // NRST: open-drain output, idle high (released)
    gpio_config_t nrst = {
        .pin_bit_mask = (1ULL << STM32_NRST_PIN),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&nrst);
    gpio_set_level(STM32_NRST_PIN, 1);  // released

    // BOOT0: push-pull output, idle low (normal boot)
    gpio_config_t boot0 = {
        .pin_bit_mask = (1ULL << STM32_BOOT0_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot0);
    gpio_set_level(STM32_BOOT0_PIN, 0); // normal boot
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    stm32_gpio_init();

    // Hardware watchdog — resets the C5 if bridge_task stops feeding it.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));

    ESP_LOGI(TAG, "Starting Klipper WiFi bridge");

    wifi_init_sta();

    // Bridge task handles its own reconnect loop and feeds the watchdog
    xTaskCreate(bridge_task, "bridge", 8192, NULL, 5, NULL);
}
