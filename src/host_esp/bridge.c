/**
 * bridge.c - SPI ↔ TCP bridge for host ESP32-C5
 *
 * spi_to_tcp task:
 *   - Blocks on spi_slave_hal_recv()
 *   - Validates frame (done by HAL already, but checks cmd too)
 *   - Extracts mcu_id and payload, calls tcp_server_send()
 *
 * tcp_to_spi path:
 *   - tcp_server calls on_tcp_rx_cb() from its receive task
 *   - Callback builds a SPI frame and calls spi_slave_hal_send()
 *
 * A small FreeRTOS queue decouples the TCP callback from SPI transmission
 * so the TCP receive task is not blocked waiting for SPI capacity.
 */

#include "bridge.h"
#include "kwm_spi.h"
#include "tcp_server.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "bridge";

/* ── Buffered TCP→SPI queue ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  cmd;
    uint8_t  mcu_id;
    uint16_t len;
    uint8_t  data[KWM_SPI_PAYLOAD_MAX];
} pending_spi_t;

#define SPI_PENDING_DEPTH   16
static QueueHandle_t s_spi_pending_queue;

/* ── TCP → SPI callback (called from TCP receive task) ───────────────────── */

static void enqueue_spi(uint8_t cmd, uint8_t mcu_id,
                         const uint8_t *data, uint16_t len) {
    pending_spi_t pkt = { .cmd = cmd, .mcu_id = mcu_id, .len = len };
    if (len) memcpy(pkt.data, data, len);
    if (xQueueSend(s_spi_pending_queue, &pkt, pdMS_TO_TICKS(50)) != pdTRUE)
        ESP_LOGW(TAG, "SPI queue full, dropping cmd=0x%02x for MCU %u", cmd, mcu_id);
}

static void on_mcu_connect(uint8_t mcu_id, const uint8_t *mac, uint8_t mac_len) {
    enqueue_spi(KWM_CMD_CONNECT, mcu_id, mac, mac_len);
}

static void on_mcu_disconnect(uint8_t mcu_id) {
    enqueue_spi(KWM_CMD_DISCONNECT, mcu_id, NULL, 0);
}

static void on_tcp_rx(uint8_t mcu_id, const uint8_t *data, uint16_t len) {
    if (len > KWM_SPI_PAYLOAD_MAX) {
        uint16_t offset = 0;
        while (offset < len) {
            uint16_t chunk = len - offset;
            if (chunk > KWM_SPI_PAYLOAD_MAX) chunk = KWM_SPI_PAYLOAD_MAX;
            enqueue_spi(KWM_CMD_DATA, mcu_id, data + offset, chunk);
            offset += chunk;
        }
    } else {
        enqueue_spi(KWM_CMD_DATA, mcu_id, data, len);
    }
}

/* ── SPI → TCP task ──────────────────────────────────────────────────────── */

static void spi_to_tcp_task(void *pvParam) {
    (void)pvParam;
    static uint8_t s_rx_frame[KWM_SPI_FRAME_LEN];
    ESP_LOGI(TAG, "spi_to_tcp task started");

    while (true) {
        esp_err_t ret = kwm_spi_recv(s_rx_frame, portMAX_DELAY);
        if (ret != ESP_OK) continue;

        const kwm_spi_frame_t *f = (const kwm_spi_frame_t *)s_rx_frame;
        uint8_t  mcu_id = kwm_mcu_id(f->mcu_id_flags);
        uint16_t plen   = kwm_be16((const uint8_t *)&f->payload_len);

        if (f->cmd == KWM_CMD_DATA && plen > 0 && plen <= KWM_SPI_PAYLOAD_MAX) {
            if (!tcp_server_mcu_connected(mcu_id)) {
                ESP_LOGD(TAG, "MCU %u not connected, dropping %u bytes from Pi",
                         mcu_id, plen);
                continue;
            }
            esp_err_t err = tcp_server_send(mcu_id, f->payload, plen);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "TCP send to MCU %u failed: %s",
                         mcu_id, esp_err_to_name(err));
        } else if (f->cmd == KWM_CMD_STATUS_REQ) {
            /* Future: send STATUS_RSP with connected MCU bitmask. */
            ESP_LOGD(TAG, "STATUS_REQ from Pi");
        }
    }
}

/* ── TCP → SPI task ──────────────────────────────────────────────────────── */

static void tcp_to_spi_task(void *pvParam) {
    (void)pvParam;
    static uint8_t s_tx_frame[KWM_SPI_FRAME_LEN];
    static uint8_t s_tx_seq;
    ESP_LOGI(TAG, "tcp_to_spi task started");

    while (true) {
        pending_spi_t pkt;
        if (xQueueReceive(s_spi_pending_queue, &pkt, portMAX_DELAY) != pdTRUE)
            continue;

        kwm_spi_frame_build(s_tx_frame,
                            (kwm_cmd_t)pkt.cmd,
                            pkt.mcu_id,
                            kwm_spi_tx_ready() ? KWM_FLAG_NONE : KWM_FLAG_MORE_DATA,
                            s_tx_seq++,
                            pkt.data,
                            pkt.len);

        esp_err_t ret = kwm_spi_send(s_tx_frame);
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "SPI send failed: %s", esp_err_to_name(ret));
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t bridge_init(void) {
    s_spi_pending_queue = xQueueCreate(SPI_PENDING_DEPTH, sizeof(pending_spi_t));
    if (!s_spi_pending_queue) return ESP_ERR_NO_MEM;

    /* Register TCP callbacks. */
    esp_err_t ret = tcp_server_init(on_tcp_rx, on_mcu_connect, on_mcu_disconnect);
    if (ret != ESP_OK) return ret;

    if (xTaskCreate(spi_to_tcp_task, "spi_to_tcp", 4096, NULL, 9, NULL) != pdPASS)
        return ESP_FAIL;

    if (xTaskCreate(tcp_to_spi_task, "tcp_to_spi", 4096, NULL, 9, NULL) != pdPASS)
        return ESP_FAIL;

    ESP_LOGI(TAG, "Bridge initialised");
    return ESP_OK;
}
