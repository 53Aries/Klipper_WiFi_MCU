/**
 * led_blink.c - Single LED status indicator (GPIO27, XIAO ESP32C5)
 *
 * A low-priority FreeRTOS task drives the LED according to the current
 * blink_state_t.  led_blink_set() is safe to call from any task.
 *
 * The XIAO ESP32C5 user LED is active-low (GPIO LOW = LED on).
 */

#include "led_blink.h"

#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_GPIO        27
#define LED_ON          0   /* active-low */
#define LED_OFF         1

static _Atomic blink_state_t s_state = BLINK_CONNECTING;

static inline void led(int level) { gpio_set_level(LED_GPIO, level); }

static void blink_task(void *pvParam)
{
    (void)pvParam;
    while (true) {
        switch (atomic_load(&s_state)) {
        case BLINK_CONNECTING:
            /* 4 Hz: 125 ms on / 125 ms off */
            led(LED_ON);  vTaskDelay(pdMS_TO_TICKS(125));
            led(LED_OFF); vTaskDelay(pdMS_TO_TICKS(125));
            break;

        case BLINK_WIFI_UP:
            /* 1 Hz: 500 ms on / 500 ms off */
            led(LED_ON);  vTaskDelay(pdMS_TO_TICKS(500));
            led(LED_OFF); vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case BLINK_TCP_CONNECTED:
            /* Solid on — check for state change every second */
            led(LED_ON);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

void led_blink_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    led(LED_OFF);

    xTaskCreate(blink_task, "led_blink", 1024, NULL, 1, NULL);
}

void led_blink_set(blink_state_t state)
{
    atomic_store(&s_state, state);
}
