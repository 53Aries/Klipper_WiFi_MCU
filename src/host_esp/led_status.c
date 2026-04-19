/**
 * led_status.c - Single LED status indicator (GPIO27, XIAO ESP32-C5)
 *
 * The XIAO user LED is active-low (GPIO LOW = LED on).
 * A low-priority FreeRTOS task drives the blink pattern.
 * led_status_set() is safe to call from any task.
 */

#include "led_status.h"

#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_GPIO  27
#define LED_ON    0   /* active-low */
#define LED_OFF   1

static _Atomic led_state_t s_state = LED_STATE_BOOTING;

static inline void led(int v)  { gpio_set_level(LED_GPIO, v); }
static inline void on(int ms)  { led(LED_ON);  vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void off(int ms) { led(LED_OFF); vTaskDelay(pdMS_TO_TICKS(ms)); }

static void blink_task(void *pvParam)
{
    (void)pvParam;
    while (true) {
        switch (atomic_load(&s_state)) {

        case LED_STATE_BOOTING:
            /* 4 Hz: 125 ms on / 125 ms off */
            on(125); off(125);
            break;

        case LED_STATE_WIFI_ERROR:
            /* SOS: · · ·  — — —  · · ·  pause */
            on(100); off(100); on(100); off(100); on(100); off(200); /* S */
            on(300); off(100); on(300); off(100); on(300); off(200); /* O */
            on(100); off(100); on(100); off(100); on(100); off(100); /* S */
            led(LED_OFF); vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        case LED_STATE_WIFI_UP:
            /* 1 Hz: 500 ms on / 500 ms off */
            on(500); off(500);
            break;

        case LED_STATE_MCU_CONNECTED:
            /* Solid on — check state every second */
            led(LED_ON);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

void led_status_init(void)
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

    xTaskCreate(blink_task, "led_status", 1024, NULL, 1, NULL);
}

void led_status_set(led_state_t state)
{
    atomic_store(&s_state, state);
}
