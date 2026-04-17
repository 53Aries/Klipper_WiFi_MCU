/**
 * kwm_uart.h - UART hardware abstraction for MCU ESP32-C5
 *
 * Drives UART1 connecting the ESP32-C5 to the STM32 Klipper MCU.
 * Default: 250000 baud, 8N1 – the standard Klipper serial rate.
 * Override baud in sdkconfig.defaults (CONFIG_KWM_UART_BAUD).
 *
 * Pins (override in sdkconfig):
 *   TX  GPIO16 → STM32 RX
 *   RX  GPIO17 ← STM32 TX
 *
 * NOTE: Named kwm_uart_* (not uart_hal_*) to avoid colliding with the
 *       ESP-IDF internal HAL component (components/hal/uart_hal.c).
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UART pin defaults. */
#ifndef KWM_UART_PORT
#  define KWM_UART_PORT  UART_NUM_1
#endif
#ifndef KWM_UART_TX_PIN
#  define KWM_UART_TX_PIN  16
#endif
#ifndef KWM_UART_RX_PIN
#  define KWM_UART_RX_PIN  17
#endif
#ifndef KWM_UART_BAUD
#  define KWM_UART_BAUD  250000
#endif
#ifndef KWM_UART_BUF_SIZE
#  define KWM_UART_BUF_SIZE  2048
#endif

/**
 * Callback invoked when data arrives from the STM32.
 * Called from the UART receive task. Buffer valid for callback duration only.
 */
typedef void (*kwm_uart_rx_cb_t)(const uint8_t *data, size_t len);

/** Initialise UART1 and start the receive task. */
esp_err_t kwm_uart_init(kwm_uart_rx_cb_t rx_cb);

/** Send bytes to the STM32. Blocks until all bytes are queued. */
esp_err_t kwm_uart_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
