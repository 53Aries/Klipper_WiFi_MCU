/**
 * bridge.c - UART ↔ TCP bridge for MCU ESP32-C5
 *
 * Data path:
 *   UART RX callback → tcp_client_send()   (STM32 → host)
 *   TCP RX callback  → uart_hal_send()     (host → STM32)
 *
 * Large UART reads are chunked to fit KWM_TCP_PAYLOAD_MAX.
 */

#include "bridge.h"
#include "kwm_uart.h"
#include "tcp_client.h"

#include <string.h>
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "bridge";

/* ── UART → TCP ──────────────────────────────────────────────────────────── */

static void on_uart_rx(const uint8_t *data, size_t len) {
    ESP_LOGI(TAG, "UART→TCP len=%u", (unsigned)len);
    size_t offset = 0;
    while (offset < len) {
        uint16_t chunk = (uint16_t)(len - offset);
        if (chunk > KWM_TCP_PAYLOAD_MAX) chunk = KWM_TCP_PAYLOAD_MAX;

        esp_err_t ret = tcp_client_send(data + offset, chunk);
        if (ret == ESP_ERR_INVALID_STATE) {
            /* Not connected – silently drop (Klipper will detect timeout). */
            break;
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "tcp_client_send failed: %s", esp_err_to_name(ret));
        }
        offset += chunk;
    }
}

/* ── TCP → UART ──────────────────────────────────────────────────────────── */

static void on_tcp_rx(const uint8_t *data, uint16_t len) {
    ESP_LOGI(TAG, "TCP→UART len=%u", len);
    esp_err_t ret = kwm_uart_send(data, len);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "kwm_uart_send failed: %s", esp_err_to_name(ret));
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t bridge_init(uint8_t mcu_id, const uint8_t *mac) {
    esp_err_t ret;

    ret = kwm_uart_init(on_uart_rx);
    if (ret != ESP_OK) return ret;

    ret = tcp_client_init(mcu_id, mac, on_tcp_rx);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MCU bridge ready (id=%u, baud=%d, uart_tx=%d, uart_rx=%d)",
             mcu_id, KWM_UART_BAUD, KWM_UART_TX_PIN, KWM_UART_RX_PIN);
    return ESP_OK;
}
