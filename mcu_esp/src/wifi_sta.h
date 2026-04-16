/**
 * wifi_sta.h - WiFi 6 station mode for MCU ESP32-C5
 *
 * Connects to the host ESP32-C5's hidden WiFi 6 AP and maintains the
 * connection with automatic reconnect on drop.
 */

#pragma once

#include "esp_err.h"
#include "kwm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked when WiFi association state changes.
 * @param connected  true = now connected, false = disconnected.
 */
typedef void (*wifi_sta_state_cb_t)(bool connected);

/**
 * Initialise WiFi station and connect to the host AP.
 * Blocks until first connection attempt completes (success or fail).
 *
 * @param state_cb  Optional callback for connection state changes.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT on initial connect failure.
 */
esp_err_t wifi_sta_init(wifi_sta_state_cb_t state_cb);

/** Returns true if currently associated with the AP. */
bool wifi_sta_connected(void);

/**
 * Block until connected (or until @p timeout_ms elapses).
 * Pass portMAX_DELAY to wait indefinitely.
 */
esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
