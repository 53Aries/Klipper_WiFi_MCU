#pragma once

/**
 * Single-LED status indicator for MCU ESP32-C5 (yellow LED on GPIO27).
 *
 * Patterns:
 *   BLINK_CONNECTING    — fast blink (4 Hz): no WiFi
 *   BLINK_WIFI_UP       — slow blink (1 Hz): WiFi ok, TCP connecting
 *   BLINK_TCP_CONNECTED — solid on:           fully operational
 */

typedef enum {
    BLINK_CONNECTING,
    BLINK_WIFI_UP,
    BLINK_TCP_CONNECTED,
} blink_state_t;

void led_blink_init(void);
void led_blink_set(blink_state_t state);
