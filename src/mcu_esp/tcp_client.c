/**
 * tcp_client.c - TCP client for MCU ESP32-C5 bridge
 *
 * Maintains a persistent TCP connection to the host ESP32-C5.
 * Uses a FreeRTOS task that:
 *   1. Waits for WiFi connectivity.
 *   2. Connects to host_ip:port.
 *   3. Sends CONNECT frame with mcu_id.
 *   4. Enters receive loop, calling rx_cb for DATA frames.
 *   5. On disconnect, waits briefly and reconnects.
 *
 * Outgoing data (tcp_client_send) is written directly to the socket
 * protected by a mutex; if the socket is closed, returns INVALID_STATE.
 */

#include "tcp_client.h"
#include "wifi_sta.h"

#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "kwm_protocol.h"

static const char *TAG = "tcp_client";

/* ── State ───────────────────────────────────────────────────────────────── */

static uint8_t           s_mcu_id;
static uint8_t           s_mac[6];
static tcp_client_rx_cb_t s_rx_cb;
static int               s_fd = -1;
static SemaphoreHandle_t s_fd_mutex;
static uint8_t           s_tx_seq;
static int               s_retry_count;

/* ── Frame reassembly (reuse same state machine logic as tcp_server) ─────── */

typedef enum {
    RX_WAIT_MAGIC0,
    RX_WAIT_MAGIC1,
    RX_WAIT_HEADER,
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC,
} rx_state_t;

typedef struct {
    rx_state_t state;
    uint8_t    hdr[KWM_TCP_HEADER_LEN];
    uint8_t    hdr_pos;
    uint8_t   *payload;
    uint16_t   payload_cap;
    uint16_t   payload_pos;
    uint16_t   payload_len;
    uint8_t    crc_buf[2];
    uint8_t    crc_pos;
} rx_ctx_t;

static void rx_ctx_init(rx_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = RX_WAIT_MAGIC0;
}

static void rx_ctx_free(rx_ctx_t *ctx) {
    free(ctx->payload);
    ctx->payload = NULL;
    ctx->payload_cap = 0;
}

static void rx_feed(rx_ctx_t *ctx, uint8_t byte) {
    switch (ctx->state) {

    case RX_WAIT_MAGIC0:
        if (byte == KWM_MAGIC_0) ctx->state = RX_WAIT_MAGIC1;
        break;

    case RX_WAIT_MAGIC1:
        if (byte == KWM_MAGIC_1) {
            ctx->hdr[0] = KWM_MAGIC_0;
            ctx->hdr[1] = KWM_MAGIC_1;
            ctx->hdr_pos = 2;
            ctx->state = RX_WAIT_HEADER;
        } else {
            ctx->state = RX_WAIT_MAGIC0;
        }
        break;

    case RX_WAIT_HEADER:
        ctx->hdr[ctx->hdr_pos++] = byte;
        if (ctx->hdr_pos == KWM_TCP_HEADER_LEN) {
            ctx->payload_len = kwm_be16(&ctx->hdr[6]);
            if (ctx->payload_len > KWM_TCP_PAYLOAD_MAX) {
                ctx->state = RX_WAIT_MAGIC0;
                break;
            }
            if (ctx->payload_len > ctx->payload_cap) {
                free(ctx->payload);
                ctx->payload = malloc(ctx->payload_len + 1);
                ctx->payload_cap = ctx->payload_len;
            }
            ctx->payload_pos = 0;
            ctx->state = (ctx->payload_len > 0) ? RX_WAIT_PAYLOAD : RX_WAIT_CRC;
        }
        break;

    case RX_WAIT_PAYLOAD:
        ctx->payload[ctx->payload_pos++] = byte;
        if (ctx->payload_pos == ctx->payload_len)
            ctx->state = RX_WAIT_CRC;
        break;

    case RX_WAIT_CRC:
        ctx->crc_buf[ctx->crc_pos++] = byte;
        if (ctx->crc_pos == 2) {
            ctx->crc_pos = 0;
            ctx->state = RX_WAIT_MAGIC0;

            uint16_t crc = 0xFFFF;
            for (int i = 0; i < KWM_TCP_HEADER_LEN; i++)
                crc = kwm_crc16_update(crc, ctx->hdr[i]);
            for (int i = 0; i < ctx->payload_len; i++)
                crc = kwm_crc16_update(crc, ctx->payload[i]);

            uint16_t recv_crc = kwm_be16(ctx->crc_buf);
            if (crc != recv_crc) {
                ESP_LOGW(TAG, "CRC mismatch (calc=%04x recv=%04x)", crc, recv_crc);
                break;
            }

            kwm_cmd_t cmd = (kwm_cmd_t)ctx->hdr[2];
            if (cmd == KWM_CMD_DATA && s_rx_cb && ctx->payload_len > 0)
                s_rx_cb(ctx->payload, ctx->payload_len);
        }
        break;
    }
}

/* ── Send ────────────────────────────────────────────────────────────────── */

esp_err_t tcp_client_send(const uint8_t *data, uint16_t len) {
    if (len > KWM_TCP_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_fd_mutex, portMAX_DELAY);
    int fd = s_fd;
    if (fd < 0) {
        xSemaphoreGive(s_fd_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Build frame on stack (header + data + CRC). */
    uint8_t hdr[KWM_TCP_HEADER_LEN];
    hdr[0] = KWM_MAGIC_0;
    hdr[1] = KWM_MAGIC_1;
    hdr[2] = KWM_CMD_DATA;
    hdr[3] = kwm_pack_id_flags(s_mcu_id, KWM_FLAG_NONE);
    hdr[4] = s_tx_seq++;
    hdr[5] = 0;
    kwm_put_be16(&hdr[6], len);

    uint16_t crc = 0xFFFF;
    for (int i = 0; i < KWM_TCP_HEADER_LEN; i++)
        crc = kwm_crc16_update(crc, hdr[i]);
    for (int i = 0; i < len; i++)
        crc = kwm_crc16_update(crc, data[i]);
    uint8_t crc_bytes[2];
    kwm_put_be16(crc_bytes, crc);

    /* Write in three parts (avoid extra malloc). */
    bool ok = (send(fd, hdr, KWM_TCP_HEADER_LEN, MSG_MORE) == KWM_TCP_HEADER_LEN) &&
              (len == 0 || send(fd, data, len, MSG_MORE) == len) &&
              (send(fd, crc_bytes, 2, 0) == 2);

    xSemaphoreGive(s_fd_mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

bool tcp_client_connected(void) {
    xSemaphoreTake(s_fd_mutex, portMAX_DELAY);
    bool c = (s_fd >= 0);
    xSemaphoreGive(s_fd_mutex);
    return c;
}

/* ── Client task ─────────────────────────────────────────────────────────── */

static void tcp_client_task(void *pvParam) {
    (void)pvParam;
    rx_ctx_t ctx;
    rx_ctx_init(&ctx);

    while (true) {
        /* Wait for WiFi. */
        ESP_LOGI(TAG, "Waiting for WiFi...");
        wifi_sta_wait_connected(portMAX_DELAY);

        /* Resolve host and connect. */
        struct sockaddr_in host_addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(KWM_TCP_PORT),
        };
        inet_pton(AF_INET, KWM_AP_IP, &host_addr.sin_addr);

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Connecting to host %s:%d...", KWM_AP_IP, KWM_TCP_PORT);
        if (connect(fd, (struct sockaddr *)&host_addr, sizeof(host_addr)) < 0) {
            ESP_LOGW(TAG, "connect() failed: errno=%d", errno);
            close(fd);
            /* Backoff: 1 s for first 5 attempts, 5 s after that. */
            vTaskDelay(pdMS_TO_TICKS(s_retry_count <= 5 ? 1000 : 5000));
            s_retry_count++;
            continue;
        }
        s_retry_count = 0;

        /* TCP_NODELAY: disable Nagle — Klipper bytes must not be buffered. */
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /* TCP keepalive: detect dead connections in ~11 s.
         * idle=5 s, interval=2 s, count=3 → fail after 5+3×2=11 s. */
        int ka = 1, ka_idle = 5, ka_intvl = 2, ka_cnt = 3;
        setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &ka,      sizeof(ka));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle, sizeof(ka_idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl,sizeof(ka_intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,  sizeof(ka_cnt));

        /* Register fd. */
        xSemaphoreTake(s_fd_mutex, portMAX_DELAY);
        s_fd = fd;
        xSemaphoreGive(s_fd_mutex);
        ESP_LOGI(TAG, "TCP connected to host (fd=%d)", fd);

        /* Send CONNECT frame: header + 6-byte MAC payload + CRC.
         * The MAC lets the host log which physical board this is. */
        {
            uint8_t frame[KWM_TCP_HEADER_LEN + KWM_MCU_ID_CONNECT_PAYLOAD_LEN
                          + KWM_TCP_CRC_LEN];
            frame[0] = KWM_MAGIC_0;
            frame[1] = KWM_MAGIC_1;
            frame[2] = KWM_CMD_CONNECT;
            frame[3] = kwm_pack_id_flags(s_mcu_id, KWM_FLAG_NONE);
            frame[4] = 0; frame[5] = 0;
            kwm_put_be16(&frame[6], KWM_MCU_ID_CONNECT_PAYLOAD_LEN);
            memcpy(&frame[8], s_mac, KWM_MCU_ID_CONNECT_PAYLOAD_LEN);
            uint16_t crc = kwm_crc16(frame,
                KWM_TCP_HEADER_LEN + KWM_MCU_ID_CONNECT_PAYLOAD_LEN);
            kwm_put_be16(&frame[KWM_TCP_HEADER_LEN + KWM_MCU_ID_CONNECT_PAYLOAD_LEN],
                         crc);
            send(fd, frame, sizeof(frame), 0);
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                     s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
            ESP_LOGI(TAG, "Sent CONNECT: mcu_id=%u  MAC=%s", s_mcu_id, mac_str);
        }

        /* Receive loop. */
        uint8_t rx_buf[512];
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (true) {
            int n = recv(fd, rx_buf, sizeof(rx_buf), 0);
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    rx_feed(&ctx, rx_buf[i]);
            } else if (n == 0) {
                ESP_LOGI(TAG, "Host closed TCP connection");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recv() error: errno=%d", errno);
                break;
            }
        }

        /* Disconnect cleanup. */
        xSemaphoreTake(s_fd_mutex, portMAX_DELAY);
        s_fd = -1;
        xSemaphoreGive(s_fd_mutex);
        close(fd);
        s_retry_count++;
        uint32_t delay_ms = (s_retry_count <= 3) ? 500 :
                            (s_retry_count <= 8) ? 2000 : 5000;
        ESP_LOGI(TAG, "TCP disconnected, reconnecting in %"PRIu32" ms (attempt %d)...",
                 delay_ms, s_retry_count);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    rx_ctx_free(&ctx);
    vTaskDelete(NULL);
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t tcp_client_init(uint8_t mcu_id, const uint8_t *mac,
                          tcp_client_rx_cb_t rx_cb) {
    s_mcu_id  = mcu_id;
    memcpy(s_mac, mac, 6);
    s_rx_cb   = rx_cb;
    s_fd      = -1;
    s_fd_mutex = xSemaphoreCreateMutex();
    if (!s_fd_mutex) return ESP_ERR_NO_MEM;

    if (xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 8, NULL) != pdPASS)
        return ESP_FAIL;

    return ESP_OK;
}
