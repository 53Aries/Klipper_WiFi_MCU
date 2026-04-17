#pragma once

/**
 * LED status indicator for host ESP32-C5 (WS2812 on GPIO27).
 *
 * States:
 *   BOOTING         — amber:  system initialising
 *   WIFI_ERROR      — red:    WiFi AP failed to start
 *   WIFI_UP         — blue:   AP running, no MCUs connected
 *   MCU_CONNECTED   — green:  ≥1 MCU has an active TCP connection
 */

typedef enum {
    LED_STATE_BOOTING,
    LED_STATE_WIFI_ERROR,
    LED_STATE_WIFI_UP,
    LED_STATE_MCU_CONNECTED,
} led_state_t;

void led_status_init(void);
void led_status_set(led_state_t state);
