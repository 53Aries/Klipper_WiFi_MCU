/**
 * wifi_ap.h - WiFi 6 soft-AP management for host ESP32-C5
 *
 * Starts a hidden WiFi 6 (802.11ax) access point. MCU ESP32-C5 boards
 * connect as stations and then open TCP connections to the host.
 */

#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "kwm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and start the WiFi 6 soft-AP.
 *
 * Configures lwIP with a static IP (KWM_AP_IP), starts DHCP server,
 * then brings up the AP with a hidden SSID. Blocks until the AP is ready.
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_init(void);

/**
 * Returns the number of MCU stations currently associated.
 */
int wifi_ap_station_count(void);

#ifdef __cplusplus
}
#endif
