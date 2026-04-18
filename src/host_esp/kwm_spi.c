/**
 * kwm_spi.c - SPI slave hardware abstraction for host ESP32-C5
 *
 * Implements full-duplex SPI slave with DMA support. Each transaction
 * transfers exactly KWM_SPI_FRAME_LEN (256) bytes in both directions.
 *
 * DATA_READY pin signals the Pi that we have outgoing data. The Pi
 * can also initiate a transaction at any time to push data to us.
 *
 * Pin assignments – compact ESP32-C5 host board (hardware FSPI pads):
 *   MOSI  GPIO3             ← Pi SPI0 MOSI pin 19
 *   MISO  GPIO8             → Pi SPI0 MISO pin 21
 *   SCLK  GPIO6  (FSPICLK) ← Pi SPI0 SCLK pin 23
 *   CS    GPIO10 (FSPICS0) ← Pi SPI0 CE0  pin 24
 *   DATA_READY GPIO25 (output) → Pi BCM GPIO25 / pin 22
 *   HANDSHAKE  GPIO26 (input)  ← Pi BCM GPIO24 / pin 18
 *
 *   GPIO3/GPIO8 use GPIO matrix routing (not dedicated FSPI IO_MUX pads)
 *   because ESP32-C5 SPI2 slave mode has a direction bug on dedicated pads.
 *
 * NOTE: Named kwm_spi_* to avoid colliding with the ESP-IDF HAL component
 *       (components/hal/spi_slave_hal.c which also defines spi_slave_hal_init).
 */

#include "kwm_spi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "kwm_spi";

/* ── Internal state ──────────────────────────────────────────────────────── */

/* DMA buffers in DRAM. esp_cache_msync() flushes/invalidates around each
 * transaction so DMA always sees the data the CPU wrote (and vice versa). */
WORD_ALIGNED_ATTR static uint8_t s_tx_dma_buf[KWM_SPI_FRAME_LEN];
WORD_ALIGNED_ATTR static uint8_t s_rx_dma_buf[KWM_SPI_FRAME_LEN];

/* Ring queues of heap-allocated frame copies. */
static QueueHandle_t s_tx_queue;   /* frames waiting to go to Pi  */
static QueueHandle_t s_rx_queue;   /* frames received from Pi      */

/* Pre-built NOOP frame sent when we have nothing to transmit. */
static uint8_t s_noop_frame[KWM_SPI_FRAME_LEN];

/* Sequence counter for outgoing SPI frames. */
static uint8_t s_tx_seq;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void spi_slave_task(void *pvParam);
static void update_data_ready(void);

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t kwm_spi_init(void) {
    esp_err_t ret;

    /* Build the standing NOOP frame once. */
    kwm_spi_frame_build(s_noop_frame, KWM_CMD_NOOP, 0, KWM_FLAG_NONE, 0, NULL, 0);

    /* Create queues. Each slot holds a pointer to a heap-allocated frame. */
    s_tx_queue = xQueueCreate(KWM_SPI_QUEUE_DEPTH, sizeof(uint8_t *));
    s_rx_queue = xQueueCreate(KWM_SPI_QUEUE_DEPTH, sizeof(uint8_t *));
    if (!s_tx_queue || !s_rx_queue) {
        ESP_LOGE(TAG, "Failed to create SPI queues");
        return ESP_ERR_NO_MEM;
    }

    /* Configure DATA_READY output. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KWM_PIN_DATA_READY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(KWM_PIN_DATA_READY, 0);

    /* Configure HANDSHAKE input (Pi drives this high when ready to receive). */
    io_conf.pin_bit_mask = (1ULL << KWM_PIN_HANDSHAKE);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;   /* default: not ready    */
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* SPI slave bus config. */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = KWM_PIN_MOSI,
        .miso_io_num   = KWM_PIN_MISO,
        .sclk_io_num   = KWM_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = KWM_SPI_FRAME_LEN,
    };

    /* SPI slave interface config.
     * Mode 0 (CPOL=0, CPHA=0): clock idles low, sample on rising edge. */
    spi_slave_interface_config_t slvcfg = {
        .mode          = 0,
        .spics_io_num  = KWM_PIN_CS,
        .queue_size    = 4,
        .flags         = 0,
        .post_setup_cb = NULL,
        .post_trans_cb = NULL,
    };

    ret = spi_slave_initialize(KWM_SPI_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_pull_mode(KWM_PIN_MOSI, GPIO_FLOATING);

    ESP_LOGI(TAG, "SPI slave initialised (MOSI=%d MISO=%d SCLK=%d CS=%d DR=%d HS=%d)",
             KWM_PIN_MOSI, KWM_PIN_MISO, KWM_PIN_SCLK, KWM_PIN_CS,
             KWM_PIN_DATA_READY, KWM_PIN_HANDSHAKE);

    /* ESP32-C5 is single-core RISC-V — use xTaskCreate (no core pinning). */
    BaseType_t rc = xTaskCreate(
        spi_slave_task, "kwm_spi", 4096, NULL, 10, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SPI task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t kwm_spi_send(const uint8_t *frame) {
    /* Heap-allocate a copy so the caller can reuse its buffer immediately. */
    uint8_t *copy = heap_caps_malloc(KWM_SPI_FRAME_LEN, MALLOC_CAP_DMA);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, frame, KWM_SPI_FRAME_LEN);

    if (xQueueSend(s_tx_queue, &copy, 0) != pdTRUE) {
        heap_caps_free(copy);
        return ESP_ERR_NO_MEM;
    }
    update_data_ready();
    return ESP_OK;
}

esp_err_t kwm_spi_recv(uint8_t *frame, uint32_t timeout_ms) {
    uint8_t *ptr = NULL;
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                     ? portMAX_DELAY
                     : pdMS_TO_TICKS(timeout_ms);

    if (xQueueReceive(s_rx_queue, &ptr, ticks) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    memcpy(frame, ptr, KWM_SPI_FRAME_LEN);
    heap_caps_free(ptr);
    return ESP_OK;
}

bool kwm_spi_tx_ready(void) {
    return uxQueueSpacesAvailable(s_tx_queue) > 0;
}

int kwm_spi_rx_pending(void) {
    return (int)uxQueueMessagesWaiting(s_rx_queue);
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void update_data_ready(void) {
    /* Assert DATA_READY iff we have at least one frame queued for the Pi. */
    int level = (uxQueueMessagesWaiting(s_tx_queue) > 0) ? 1 : 0;
    gpio_set_level(KWM_PIN_DATA_READY, level);
}

/* ── SPI driver task ─────────────────────────────────────────────────────── */

static void spi_slave_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "SPI slave task started");

    while (true) {
        /* Decide what to transmit.
         * Dequeue a frame from TX queue, or fall back to NOOP. */
        uint8_t *tx_ptr = NULL;
        bool     have_tx = (xQueuePeek(s_tx_queue, &tx_ptr, 0) == pdTRUE);

        if (have_tx) {
            xQueueReceive(s_tx_queue, &tx_ptr, 0);
            memcpy(s_tx_dma_buf, tx_ptr, KWM_SPI_FRAME_LEN);
            heap_caps_free(tx_ptr);
            /* Keep DATA_READY asserted: real frame is loaded, Pi must clock it out.
             * Do NOT call update_data_ready() here — that would de-assert because
             * the queue is now empty, causing the Pi to miss this frame. */
            gpio_set_level(KWM_PIN_DATA_READY, 1);
        } else {
            memcpy(s_tx_dma_buf, s_noop_frame, KWM_SPI_FRAME_LEN);
            s_tx_dma_buf[4] = s_tx_seq++;
            uint16_t crc = kwm_crc16(s_tx_dma_buf, KWM_SPI_FRAME_LEN - 2);
            kwm_put_be16(s_tx_dma_buf + KWM_SPI_FRAME_LEN - 2, crc);
            gpio_set_level(KWM_PIN_DATA_READY, 0);
        }

        memset(s_rx_dma_buf, 0, KWM_SPI_FRAME_LEN);

        spi_slave_transaction_t t = {
            .length    = KWM_SPI_FRAME_LEN * 8,  /* bits */
            .tx_buffer = s_tx_dma_buf,
            .rx_buffer = s_rx_dma_buf,
        };

        /* Block until Pi initiates a transaction (CS falling edge).
         * IDF 5.x spi_slave_transmit handles cache coherency internally
         * for static DRAM buffers — no manual esp_cache_msync needed. */
        esp_err_t ret = spi_slave_transmit(KWM_SPI_HOST, &t, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "spi_slave_transmit error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        ESP_LOGI(TAG, "SPI txn done: rx[0:4]=%02x%02x%02x%02x",
                 s_rx_dma_buf[0], s_rx_dma_buf[1], s_rx_dma_buf[2], s_rx_dma_buf[3]);

        /* Transaction complete — now reflect true queue state on DATA_READY. */
        update_data_ready();

        /* Validate and enqueue received frame (ignore NOOPs and bad CRCs). */
        if (kwm_spi_frame_valid(s_rx_dma_buf)) {
            const kwm_spi_frame_t *f = (const kwm_spi_frame_t *)s_rx_dma_buf;
            if (f->cmd != KWM_CMD_NOOP) {
                uint8_t *copy = heap_caps_malloc(KWM_SPI_FRAME_LEN, MALLOC_CAP_DMA);
                if (copy) {
                    memcpy(copy, s_rx_dma_buf, KWM_SPI_FRAME_LEN);
                    if (xQueueSend(s_rx_queue, &copy, pdMS_TO_TICKS(10)) != pdTRUE) {
                        ESP_LOGW(TAG, "RX queue full, dropping frame");
                        heap_caps_free(copy);
                    }
                }
            }
        } else {
            ESP_LOGD(TAG, "Received invalid SPI frame (bad magic/CRC)");
        }
    }
}
