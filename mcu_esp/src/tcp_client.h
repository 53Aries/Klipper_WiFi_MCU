/**
 * tcp_client.h - TCP client connecting MCU ESP32-C5 to host ESP32-C5
 *
 * Connects to KWM_AP_IP:KWM_TCP_PORT, sends a KWM_CMD_CONNECT frame with
 * this MCU's ID, then bridges data frames bidirectionally.
 *
 * Reconnects automatically when the TCP connection drops.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked when a DATA payload arrives from the host.
 * Called from the TCP receive task. Buffer valid for callback duration.
 */
typedef void (*tcp_client_rx_cb_t)(const uint8_t *data, uint16_t len);

/**
 * Start the TCP client task.
 *
 * @param mcu_id  This board's MCU ID (0-7); sent in CONNECT frame.
 * @param rx_cb   Called on incoming DATA from host.
 * @return ESP_OK or error.
 */
esp_err_t tcp_client_init(uint8_t mcu_id, tcp_client_rx_cb_t rx_cb);

/**
 * Send raw serial bytes to the host ESP.
 * Wraps bytes in a KWM TCP frame.
 *
 * @param data  Bytes to send.
 * @param len   Length (must be <= KWM_TCP_PAYLOAD_MAX).
 * @return ESP_OK, ESP_ERR_INVALID_STATE if not connected, or error.
 */
esp_err_t tcp_client_send(const uint8_t *data, uint16_t len);

/** Returns true if the TCP connection to the host is currently active. */
bool tcp_client_connected(void);

#ifdef __cplusplus
}
#endif
