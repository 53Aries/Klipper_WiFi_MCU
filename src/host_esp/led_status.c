/**
 * led_status.c - WS2812 RGB status LED on GPIO27 (host ESP32-C5)
 *
 * Uses the ESP-IDF 5.x RMT TX driver with the bytes encoder.
 * All public functions are safe to call from any FreeRTOS task.
 *
 * Timing at 10 MHz RMT clock (100 ns/tick):
 *   bit-0:  HIGH 300 ns (3 ticks) | LOW 900 ns (9 ticks)
 *   bit-1:  HIGH 600 ns (6 ticks) | LOW 600 ns (6 ticks)
 *   reset:  pin stays LOW between calls (EOT level = 0, >50 µs guaranteed)
 */

#include "led_status.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define LED_GPIO          27
#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)   /* 10 MHz → 100 ns/tick */

static const char *TAG = "led_status";

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_enc  = NULL;
static SemaphoreHandle_t    s_lock = NULL;

static const rmt_transmit_config_t s_tx_cfg = {
    .loop_count = 0,
    .flags.eot_level = 0,   /* pin goes LOW after transmission — WS2812 reset */
};

/* Send one GRB pixel (WS2812 wire order is G, R, B). */
static void send_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &s_tx_cfg);
    rmt_tx_wait_all_done(s_chan, 50 /* ms */);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void led_status_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed");
        return;
    }

    rmt_bytes_encoder_config_t enc_cfg = {
        /* bit-0: 300 ns high, 900 ns low */
        .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
        /* bit-1: 600 ns high, 600 ns low */
        .bit1 = { .duration0 = 6, .level0 = 1, .duration1 = 6, .level1 = 0 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed");
        return;
    }

    rmt_enable(s_chan);
    ESP_LOGI(TAG, "LED on GPIO%d ready", LED_GPIO);
}

void led_status_set(led_state_t state)
{
    if (!s_chan || !s_enc || !s_lock) return;

    uint8_t r, g, b;
    switch (state) {
    case LED_STATE_BOOTING:        r = 30;  g = 10;  b = 0;   break; /* amber  */
    case LED_STATE_WIFI_ERROR:     r = 80;  g = 0;   b = 0;   break; /* red    */
    case LED_STATE_WIFI_UP:        r = 0;   g = 0;   b = 60;  break; /* blue   */
    case LED_STATE_MCU_CONNECTED:  r = 0;   g = 60;  b = 0;   break; /* green  */
    default:                       r = 0;   g = 0;   b = 0;   break; /* off    */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    send_pixel(r, g, b);
    xSemaphoreGive(s_lock);
}
