/**
 * bridge.h - UART ↔ TCP bridge for MCU ESP32-C5
 *
 * Ties together UART HAL and TCP client:
 *   STM32 → UART → ESP → TCP → host ESP → SPI → Pi5
 *   Pi5 → SPI → host ESP → TCP → ESP → UART → STM32
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the bridge.
 *
 * @param mcu_id  This board's MCU ID (0-7), derived from MAC address.
 * @param mac     This board's 6-byte MAC address (passed to TCP CONNECT frame).
 * @return ESP_OK or error.
 */
esp_err_t bridge_init(uint8_t mcu_id, const uint8_t *mac);

#ifdef __cplusplus
}
#endif
