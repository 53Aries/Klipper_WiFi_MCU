/**
 * bridge.h - UART ↔ TCP bridge for MCU ESP32-C5
 *
 * Ties together UART HAL and TCP client:
 *   STM32 → UART → ESP → TCP → host ESP → SPI → Pi5
 *   Pi5 → SPI → host ESP → TCP → ESP → UART → STM32
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the bridge.
 *
 * Must be called after uart_hal_init() and tcp_client_init().
 * Registers callbacks so data flows automatically.
 *
 * @param mcu_id  This board's MCU ID.
 * @return ESP_OK or error.
 */
esp_err_t bridge_init(uint8_t mcu_id);

#ifdef __cplusplus
}
#endif
