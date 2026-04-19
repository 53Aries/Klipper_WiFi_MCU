#pragma once

/**
 * LED status indicator for host XIAO ESP32-C5 (active-low GPIO27).
 *
 * States and blink patterns:
 *   BOOTING         — fast blink  4 Hz  (125 ms on / 125 ms off)
 *   WIFI_ERROR      — SOS: 3×short, 3×long, 3×short, 1 s pause
 *   WIFI_UP         — slow blink  1 Hz  (500 ms on / 500 ms off)
 *   MCU_CONNECTED   — solid on
 */

typedef enum {
    LED_STATE_BOOTING,
    LED_STATE_WIFI_ERROR,
    LED_STATE_WIFI_UP,
    LED_STATE_MCU_CONNECTED,
} led_state_t;

void led_status_init(void);
void led_status_set(led_state_t state);
