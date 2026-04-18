/**
 * tcp_server.h - TCP server accepting connections from MCU ESP32-C5 bridges
 *
 * Listens on KWM_TCP_PORT. When a MCU ESP connects it sends a KWM_CMD_CONNECT
 * frame identifying its MCU ID. The server then tracks the connection and
 * routes data frames based on mcu_id.
 *
 * Data flow (MCU → Pi):
 *   TCP recv → build SPI frame → tcp_server_send_to_pi()
 *
 * Data flow (Pi → MCU):
 *   tcp_server_send_to_mcu(mcu_id, data, len) → TCP send
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked when a DATA frame arrives from a MCU.
 *
 * Called from the MCU connection task (not from the listener).
 * The buffer is valid only for the duration of the callback.
 *
 * @param mcu_id  Source MCU ID (0-7).
 * @param data    Pointer to raw payload bytes.
 * @param len     Payload length.
 */
typedef void (*tcp_server_rx_cb_t)(uint8_t mcu_id,
                                   const uint8_t *data, uint16_t len);

/** Callback fired when a MCU TCP connection is identified. mac/mac_len is the
 *  CONNECT frame payload (6-byte MAC), valid only for the call duration. */
typedef void (*tcp_server_connect_cb_t)(uint8_t mcu_id,
                                        const uint8_t *mac, uint8_t mac_len);

/** Callback fired when a MCU TCP connection closes. */
typedef void (*tcp_server_disconnect_cb_t)(uint8_t mcu_id);

/**
 * Start the TCP listener task.
 *
 * @param rx_cb          Called whenever a DATA frame arrives from any MCU.
 * @param connect_cb     Called when a MCU identifies itself (may be NULL).
 * @param disconnect_cb  Called when a MCU connection closes (may be NULL).
 * @return ESP_OK or error.
 */
esp_err_t tcp_server_init(tcp_server_rx_cb_t rx_cb,
                           tcp_server_connect_cb_t connect_cb,
                           tcp_server_disconnect_cb_t disconnect_cb);

/**
 * Send raw serial data to a specific MCU.
 *
 * Wraps data in a KWM TCP frame and writes to the MCU's open socket.
 * Returns ESP_ERR_NOT_FOUND if MCU is not connected.
 *
 * @param mcu_id  Destination MCU ID (0-7).
 * @param data    Raw bytes to send.
 * @param len     Number of bytes (must be <= KWM_TCP_PAYLOAD_MAX).
 * @return ESP_OK or error.
 */
esp_err_t tcp_server_send(uint8_t mcu_id, const uint8_t *data, uint16_t len);

/** Returns true if the given MCU ID has an active TCP connection. */
bool tcp_server_mcu_connected(uint8_t mcu_id);

#ifdef __cplusplus
}
#endif
