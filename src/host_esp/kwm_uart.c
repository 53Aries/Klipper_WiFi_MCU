/**
 * kwm_uart.c - UART transport HAL for host ESP32-C5 ↔ Pi5
 *
 * Uses fixed 256-byte frames (identical format to former SPI frames) over
 * UART1 at KWM_HOST_UART_BAUD. Fixed frame size keeps the Pi-side simple:
 * read exactly 256 bytes, validate magic+CRC.
 *
 * RX resync: if the received frame fails magic/CRC check, discard byte-by-byte
 * until 0xAB 0xCD is found, then re-align. Handles noise/startup framing.
 */

#include "kwm_uart.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "kwm_uart";

#define UART_PORT       UART_NUM_1
#define UART_RX_BUF     (KWM_SPI_FRAME_LEN * 8)

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_rx_queue;

/* ── TX task ─────────────────────────────────────────────────────────────── */

static void uart_tx_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "TX task started");
    while (true) {
        uint8_t *ptr = NULL;
        if (xQueueReceive(s_tx_queue, &ptr, portMAX_DELAY) == pdTRUE) {
            uart_write_bytes(UART_PORT, (const char *)ptr, KWM_SPI_FRAME_LEN);
            free(ptr);
        }
    }
}

/* ── RX task ─────────────────────────────────────────────────────────────── */

static void uart_rx_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "RX task started");

    static uint8_t buf[KWM_SPI_FRAME_LEN];
    int pos = 0;

    while (true) {
        /* Fill the rest of the frame buffer. */
        int n = uart_read_bytes(UART_PORT, buf + pos,
                                KWM_SPI_FRAME_LEN - pos, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        pos += n;

        if (pos < KWM_SPI_FRAME_LEN) continue;   /* not a full frame yet */

        /* Full frame received. */
        if (kwm_spi_frame_valid(buf)) {
            const kwm_spi_frame_t *f = (const kwm_spi_frame_t *)buf;
            if (f->cmd != KWM_CMD_NOOP) {
                uint8_t *copy = malloc(KWM_SPI_FRAME_LEN);
                if (copy) {
                    memcpy(copy, buf, KWM_SPI_FRAME_LEN);
                    if (xQueueSend(s_rx_queue, &copy, pdMS_TO_TICKS(10)) != pdTRUE) {
                        ESP_LOGW(TAG, "RX queue full, dropping frame");
                        free(copy);
                    }
                }
            }
            pos = 0;
        } else {
            /* Bad frame — scan forward for next magic bytes to resync. */
            ESP_LOGD(TAG, "Bad frame (magic/CRC), resyncing...");
            int resync = 1;
            for (int i = 1; i < KWM_SPI_FRAME_LEN - 1; i++) {
                if (buf[i] == KWM_MAGIC_0 && buf[i + 1] == KWM_MAGIC_1) {
                    resync = i;
                    break;
                }
            }
            pos = KWM_SPI_FRAME_LEN - resync;
            memmove(buf, buf + resync, pos);
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t kwm_uart_init(void) {
    s_tx_queue = xQueueCreate(KWM_UART_QUEUE_DEPTH, sizeof(uint8_t *));
    s_rx_queue = xQueueCreate(KWM_UART_QUEUE_DEPTH, sizeof(uint8_t *));
    if (!s_tx_queue || !s_rx_queue) return ESP_ERR_NO_MEM;

    uart_config_t cfg = {
        .baud_rate           = KWM_HOST_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 KWM_HOST_UART_TX_PIN,
                                 KWM_HOST_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART1 ready (TX=%d RX=%d baud=%d)",
             KWM_HOST_UART_TX_PIN, KWM_HOST_UART_RX_PIN, KWM_HOST_UART_BAUD);

    xTaskCreate(uart_tx_task, "kwm_uart_tx", 3072, NULL, 10, NULL);
    xTaskCreate(uart_rx_task, "kwm_uart_rx", 3072, NULL, 10, NULL);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t kwm_uart_send(const uint8_t *frame) {
    uint8_t *copy = malloc(KWM_SPI_FRAME_LEN);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, frame, KWM_SPI_FRAME_LEN);
    if (xQueueSend(s_tx_queue, &copy, 0) != pdTRUE) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kwm_uart_recv(uint8_t *frame, uint32_t timeout_ms) {
    uint8_t *ptr = NULL;
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                     ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_rx_queue, &ptr, ticks) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    memcpy(frame, ptr, KWM_SPI_FRAME_LEN);
    free(ptr);
    return ESP_OK;
}

bool kwm_uart_tx_ready(void) {
    return uxQueueSpacesAvailable(s_tx_queue) > 0;
}

int kwm_uart_rx_pending(void) {
    return (int)uxQueueMessagesWaiting(s_rx_queue);
}
