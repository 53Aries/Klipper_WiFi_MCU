/**
 * kwm_uart.c - UART transport HAL for host ESP32-C5 ↔ Pi5
 *
 * TODO: implement full UART send/receive.
 * Stub compiles and boots; bridge tasks will block on empty queues.
 */

#include "kwm_uart.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "kwm_uart";

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_rx_queue;

esp_err_t kwm_uart_init(void) {
    s_tx_queue = xQueueCreate(KWM_UART_QUEUE_DEPTH, sizeof(uint8_t *));
    s_rx_queue = xQueueCreate(KWM_UART_QUEUE_DEPTH, sizeof(uint8_t *));
    if (!s_tx_queue || !s_rx_queue) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "UART transport stub init (TX=%d RX=%d baud=%d) — NOT YET IMPLEMENTED",
             KWM_HOST_UART_TX_PIN, KWM_HOST_UART_RX_PIN, KWM_HOST_UART_BAUD);
    return ESP_OK;
}

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
