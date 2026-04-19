/**
 * kwm_uart.h - UART transport HAL for host ESP32-C5 ↔ Pi5
 *
 * Replaces the SPI slave transport. The Pi talks to the ESP over a
 * simple UART at KWM_HOST_UART_BAUD (default 1 Mbaud).
 *
 * Frame format is identical to the SPI frame (magic + header + payload +
 * CRC16), but frames are variable-length: only HEADER_LEN + payload_len +
 * CRC_LEN bytes are transmitted — no padding to 256 bytes.
 *
 * Pin assignments – Seeed XIAO ESP32-C5 (host board):
 *   UART1 TX  GPIO11 (D6) → Pi BCM15 (pin 10, RXD)
 *   UART1 RX  GPIO12 (D7) ← Pi BCM14 (pin  8, TXD)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "kwm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KWM_HOST_UART_TX_PIN
#  define KWM_HOST_UART_TX_PIN  11
#endif

#ifndef KWM_HOST_UART_RX_PIN
#  define KWM_HOST_UART_RX_PIN  12
#endif

#ifndef KWM_HOST_UART_BAUD
#  define KWM_HOST_UART_BAUD    1000000
#endif

#ifndef KWM_UART_QUEUE_DEPTH
#  define KWM_UART_QUEUE_DEPTH  16
#endif

esp_err_t kwm_uart_init(void);

esp_err_t kwm_uart_send(const uint8_t *frame);
esp_err_t kwm_uart_recv(uint8_t *frame, uint32_t timeout_ms);

bool kwm_uart_tx_ready(void);
int  kwm_uart_rx_pending(void);

#ifdef __cplusplus
}
#endif
