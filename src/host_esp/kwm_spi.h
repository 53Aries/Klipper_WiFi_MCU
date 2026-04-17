/**
 * kwm_spi.h - SPI slave hardware abstraction for host ESP32-C5
 *
 * The Pi5 is the SPI master. This module configures the ESP32-C5 as a
 * full-duplex SPI slave (mode 3, 256-byte fixed frames).
 *
 * Transaction flow:
 *   1. When ESP has data to send it asserts DATA_READY (GPIO high).
 *   2. Pi detects DATA_READY (interrupt or poll), initiates SPI transfer.
 *   3. Both sides simultaneously transmit their 256-byte frame.
 *   4. ESP de-asserts DATA_READY after the transfer (or keeps it high if
 *      more frames are pending).
 *
 * The HAL exposes a simple send/receive queue interface; the bridge layer
 * coordinates between SPI and TCP.
 *
 * NOTE: Named kwm_spi_* (not spi_slave_hal_*) to avoid colliding with the
 *       ESP-IDF internal HAL component (components/hal/spi_slave_hal.c).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "kwm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration (override in sdkconfig.defaults) ─────────────────────── */
#ifndef KWM_SPI_CLOCK_HZ
#  define KWM_SPI_CLOCK_HZ   (10 * 1000 * 1000)  /* 10 MHz, conservative   */
#endif

#ifndef KWM_SPI_QUEUE_DEPTH
#  define KWM_SPI_QUEUE_DEPTH  8                  /* frames in each direction*/
#endif

/* ── Initialisation ──────────────────────────────────────────────────────── */

/**
 * Initialise the SPI slave peripheral and GPIO control lines.
 *
 * Must be called once before any other kwm_spi_* function.
 * Spawns an internal FreeRTOS task that drives SPI transactions.
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t kwm_spi_init(void);

/* ── Data transfer ───────────────────────────────────────────────────────── */

/**
 * Enqueue a 256-byte SPI frame for transmission to the Pi.
 *
 * Does not block. Returns ESP_ERR_NO_MEM if the TX queue is full.
 * The caller should back-pressure upstream (stop pulling from TCP) when full.
 *
 * @param frame  Pointer to KWM_SPI_FRAME_LEN bytes; copied into internal buffer.
 * @return ESP_OK or ESP_ERR_NO_MEM.
 */
esp_err_t kwm_spi_send(const uint8_t *frame);

/**
 * Dequeue a received 256-byte SPI frame from the Pi.
 *
 * Blocks for up to @p timeout_ms milliseconds. Returns ESP_ERR_TIMEOUT if
 * no frame arrives in time.
 *
 * @param frame       Caller-supplied KWM_SPI_FRAME_LEN byte buffer.
 * @param timeout_ms  Block duration; portMAX_DELAY for indefinite wait.
 * @return ESP_OK or ESP_ERR_TIMEOUT.
 */
esp_err_t kwm_spi_recv(uint8_t *frame, uint32_t timeout_ms);

/* ── Status ──────────────────────────────────────────────────────────────── */

/** Returns true if the TX queue has space for at least one more frame. */
bool kwm_spi_tx_ready(void);

/** Returns the number of received frames waiting in the RX queue. */
int  kwm_spi_rx_pending(void);

#ifdef __cplusplus
}
#endif
