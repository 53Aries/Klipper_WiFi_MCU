/**
 * bridge.h - SPI ↔ TCP bridge for host ESP32-C5
 *
 * Ties together the SPI slave HAL and TCP server:
 *
 *   Pi  ←→  SPI slave HAL  ←→  bridge  ←→  TCP server  ←→  MCU ESPs
 *
 * The bridge runs two tasks:
 *   spi_to_tcp: reads SPI frames from the HAL, decodes them, routes the
 *               payload to the correct MCU TCP connection.
 *   tcp_to_spi: reads DATA callbacks from the TCP server, encodes them
 *               into SPI frames, hands them to the HAL for transmission.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and start the bridge.
 *
 * Must be called after spi_slave_hal_init() and tcp_server_init().
 *
 * @return ESP_OK or error.
 */
esp_err_t bridge_init(void);

#ifdef __cplusplus
}
#endif
