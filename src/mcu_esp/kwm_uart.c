/**
 * kwm_uart.c - UART bridge to STM32 Klipper MCU
 *
 * Uses ESP-IDF UART driver with a ring buffer.
 * A dedicated task reads from the driver and fires rx_cb.
 *
 * NOTE: Named kwm_uart_* to avoid colliding with ESP-IDF HAL component
 *       (components/hal/uart_hal.c which also defines uart_hal_init).
 */

#include "kwm_uart.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "kwm_uart";

static kwm_uart_rx_cb_t s_rx_cb;

/* ── UART receive task ───────────────────────────────────────────────────── */

static void uart_rx_task(void *pvParam) {
    (void)pvParam;
    static uint8_t rx_buf[KWM_UART_BUF_SIZE];
    ESP_LOGI(TAG, "UART RX task started (port=%d baud=%d TX=%d RX=%d)",
             KWM_UART_PORT, KWM_UART_BAUD, KWM_UART_TX_PIN, KWM_UART_RX_PIN);

    while (true) {
        /* uart_read_bytes blocks up to 20 ms waiting for data. */
        int n = uart_read_bytes(KWM_UART_PORT, rx_buf, sizeof(rx_buf),
                                pdMS_TO_TICKS(20));
        if (n > 0 && s_rx_cb) {
            s_rx_cb(rx_buf, (size_t)n);
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t kwm_uart_init(kwm_uart_rx_cb_t rx_cb) {
    s_rx_cb = rx_cb;

    uart_config_t cfg = {
        .baud_rate  = KWM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(KWM_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(KWM_UART_PORT,
                                 KWM_UART_TX_PIN, KWM_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(KWM_UART_PORT,
                                        KWM_UART_BUF_SIZE * 2,  /* rx buf */
                                        KWM_UART_BUF_SIZE * 2,  /* tx buf */
                                        0, NULL, 0));

    if (xTaskCreate(uart_rx_task, "kwm_uart_rx", 3072, NULL, 10, NULL) != pdPASS)
        return ESP_FAIL;

    ESP_LOGI(TAG, "UART init OK");
    return ESP_OK;
}

/* ── Send ────────────────────────────────────────────────────────────────── */

esp_err_t kwm_uart_send(const uint8_t *data, size_t len) {
    int written = uart_write_bytes(KWM_UART_PORT, data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGW(TAG, "uart_write_bytes short write: %d/%zu", written, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}
