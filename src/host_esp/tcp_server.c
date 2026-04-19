/**
 * tcp_server.c - TCP server for MCU ESP32-C5 bridge connections
 *
 * Architecture:
 *   - Listener task: accept() loop, spawns a per-MCU connection task.
 *   - Connection task (one per MCU): reads TCP stream, reassembles frames,
 *     invokes rx_cb for DATA frames.
 *   - tcp_server_send(): called from bridge task to push data to a MCU.
 *
 * Frame reassembly uses a simple state machine:
 *   WAIT_MAGIC → WAIT_HEADER → WAIT_PAYLOAD → WAIT_CRC → dispatch
 */

#include "tcp_server.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "kwm_protocol.h"
#include "led_status.h"

static const char *TAG = "tcp_server";

/* ── Per-MCU connection state ────────────────────────────────────────────── */

typedef struct {
    int            fd;              /* socket fd; -1 = not connected        */
    SemaphoreHandle_t tx_mutex;    /* guards writes to fd                   */
    bool           active;
} mcu_conn_t;

static mcu_conn_t  s_conns[KWM_MAX_MCU];
static tcp_server_rx_cb_t         s_rx_cb;
static tcp_server_connect_cb_t    s_connect_cb;
static tcp_server_disconnect_cb_t s_disconnect_cb;
static _Atomic int s_mcu_count = 0;

/* ── Frame reassembly state ──────────────────────────────────────────────── */

typedef enum {
    RX_WAIT_MAGIC0,
    RX_WAIT_MAGIC1,
    RX_WAIT_HEADER,   /* collect remaining 6 header bytes after magic     */
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC,
} rx_state_t;

typedef struct {
    rx_state_t   state;
    uint8_t      hdr[KWM_TCP_HEADER_LEN];
    uint8_t      hdr_pos;
    uint8_t     *payload;
    uint16_t     payload_cap;
    uint16_t     payload_pos;
    uint16_t     payload_len;
    uint8_t      crc_buf[2];
    uint8_t      crc_pos;
    bool         connect_seen;  /* set only after a CRC-valid CONNECT frame */
} rx_ctx_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void rx_ctx_init(rx_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = RX_WAIT_MAGIC0;
    ctx->payload = NULL;
    ctx->payload_cap = 0;
}

static void rx_ctx_free(rx_ctx_t *ctx) {
    free(ctx->payload);
    ctx->payload = NULL;
}

/**
 * Feed one byte into the reassembly state machine.
 * Returns true when a complete, valid frame is ready in ctx->hdr / ctx->payload.
 */
static bool rx_feed(rx_ctx_t *ctx, uint8_t byte, const tcp_server_rx_cb_t rx_cb) {
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
                ESP_LOGW(TAG, "TCP frame payload_len %u > max, resync",
                         ctx->payload_len);
                ctx->state = RX_WAIT_MAGIC0;
                break;
            }
            /* Allocate/grow payload buffer. */
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

            /* Verify CRC: init 0xFFFF, feed header then payload. */
            uint16_t crc = 0xFFFF;
            for (int i = 0; i < KWM_TCP_HEADER_LEN; i++)
                crc = kwm_crc16_update(crc, ctx->hdr[i]);
            for (int i = 0; i < ctx->payload_len; i++)
                crc = kwm_crc16_update(crc, ctx->payload[i]);

            uint16_t recv_crc = kwm_be16(ctx->crc_buf);
            if (crc != recv_crc) {
                ESP_LOGW(TAG, "TCP frame CRC mismatch (calc=%04x recv=%04x)", crc, recv_crc);
                break;
            }

            /* Dispatch. */
            kwm_cmd_t cmd   = (kwm_cmd_t)ctx->hdr[2];
            uint8_t mcu_id  = kwm_mcu_id(ctx->hdr[3]);

            if (cmd == KWM_CMD_CONNECT)
                ctx->connect_seen = true;

            if (cmd == KWM_CMD_DATA && rx_cb && ctx->payload_len > 0) {
                rx_cb(mcu_id, ctx->payload, ctx->payload_len);
            } else if (cmd == KWM_CMD_CONNECT) {
                /* Log the MAC address if it was included in the payload. */
                if (ctx->payload_len >= KWM_MCU_ID_CONNECT_PAYLOAD_LEN) {
                    const uint8_t *m = ctx->payload;
                    char mac_str[18];
                    snprintf(mac_str, sizeof(mac_str),
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             m[0], m[1], m[2], m[3], m[4], m[5]);
                    ESP_LOGI(TAG, "MCU %u connected  MAC=%s  → /dev/kwm%u",
                             mcu_id, mac_str, mcu_id);
                } else {
                    ESP_LOGI(TAG, "MCU %u connected (no MAC in payload)", mcu_id);
                }
            }
        }
        break;
    }
    return false; /* state machine driven by side effects */
}

/* ── TCP send ────────────────────────────────────────────────────────────── */

esp_err_t tcp_server_send(uint8_t mcu_id, const uint8_t *data, uint16_t len) {
    if (mcu_id >= KWM_MAX_MCU) return ESP_ERR_INVALID_ARG;
    if (len > KWM_TCP_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    mcu_conn_t *conn = &s_conns[mcu_id];

    xSemaphoreTake(conn->tx_mutex, portMAX_DELAY);
    if (conn->fd < 0) {
        xSemaphoreGive(conn->tx_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build frame per-send on the heap – avoids a shared scratch buffer that
     * would serialize sends to different MCUs. */
    size_t total = KWM_TCP_HEADER_LEN + len + KWM_TCP_CRC_LEN;
    uint8_t *frame = malloc(total);
    if (!frame) {
        xSemaphoreGive(conn->tx_mutex);
        return ESP_ERR_NO_MEM;
    }

    frame[0] = KWM_MAGIC_0;
    frame[1] = KWM_MAGIC_1;
    frame[2] = KWM_CMD_DATA;
    frame[3] = kwm_pack_id_flags(mcu_id, KWM_FLAG_NONE);
    frame[4] = 0;   /* seq – TODO: track per-MCU */
    frame[5] = 0;
    kwm_put_be16(&frame[6], len);
    if (len && data)
        memcpy(&frame[8], data, len);
    uint16_t crc = kwm_crc16(frame, KWM_TCP_HEADER_LEN + len);
    kwm_put_be16(&frame[KWM_TCP_HEADER_LEN + len], crc);

    /* Blocking send – SO_SNDTIMEO set on the socket limits wait to 200 ms.
     * Using MSG_DONTWAIT here would silently drop data; Klipper cannot
     * tolerate any byte loss. */
    int sent = send(conn->fd, frame, total, 0);
    free(frame);
    xSemaphoreGive(conn->tx_mutex);

    if (sent != (int)total) {
        ESP_LOGW(TAG, "TCP send to MCU %u incomplete (%d/%d): errno=%d",
                 mcu_id, sent, (int)total, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Per-MCU receive task ────────────────────────────────────────────────── */

typedef struct {
    int     fd;
    uint8_t mcu_id;  /* provisional until CONNECT frame received */
} conn_task_arg_t;

static void mcu_conn_task(void *pvParam) {
    conn_task_arg_t *arg = pvParam;
    int fd = arg->fd;
    free(arg);

    uint8_t  rx_buf[256];
    rx_ctx_t ctx;
    rx_ctx_init(&ctx);

    uint8_t mcu_id = KWM_MAX_MCU;  /* unknown until CONNECT arrives */

    /* Peek at the first frame to learn this MCU's ID.
     * We wait up to 5 s; if no CONNECT, close and exit. */
    bool identified = false;
    const TickType_t id_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (!identified && xTaskGetTickCount() < id_deadline) {
        int n = recv(fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            rx_feed(&ctx, rx_buf[i], NULL);
            if (ctx.connect_seen) {
                mcu_id = kwm_mcu_id(ctx.hdr[3]);
                if (mcu_id < KWM_MAX_MCU) {
                    identified = true;
                    break;
                }
                ctx.connect_seen = false;  /* bad id, keep waiting */
            }
        }
    }

    if (!identified || mcu_id >= KWM_MAX_MCU) {
        ESP_LOGW(TAG, "Connection did not identify MCU, closing fd=%d", fd);
        close(fd);
        rx_ctx_free(&ctx);
        vTaskDelete(NULL);
        return;
    }

    /* Register the connection. */
    mcu_conn_t *conn = &s_conns[mcu_id];
    xSemaphoreTake(conn->tx_mutex, portMAX_DELAY);
    if (conn->fd >= 0) {
        ESP_LOGW(TAG, "MCU %u already connected, replacing", mcu_id);
        close(conn->fd);
    }
    conn->fd     = fd;
    conn->active = true;
    xSemaphoreGive(conn->tx_mutex);

    if (atomic_fetch_add(&s_mcu_count, 1) == 0)
        led_status_set(LED_STATE_MCU_CONNECTED);

    ESP_LOGI(TAG, "MCU %u identified and registered on fd=%d", mcu_id, fd);

    if (s_connect_cb)
        s_connect_cb(mcu_id, ctx.payload,
                     (uint8_t)(ctx.payload_len < 255 ? ctx.payload_len : 255));

    /* TCP_NODELAY: disable Nagle — Klipper bytes must not be buffered.
     * Without this, lwIP waits up to 40 ms for more data before sending. */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* SO_SNDTIMEO: bound blocking sends to 200 ms. Without this, a slow
     * MCU could stall the bridge task indefinitely. */
    struct timeval sndtv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv));

    /* TCP keepalive: detect dead MCU connections in ~11 s. */
    int ka = 1, ka_idle = 5, ka_intvl = 2, ka_cnt = 3;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &ka,      sizeof(ka));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle, sizeof(ka_idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl,sizeof(ka_intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,  sizeof(ka_cnt));

    /* Receive timeout for the main loop. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Main receive loop. */
    while (true) {
        int n = recv(fd, rx_buf, sizeof(rx_buf), 0);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                rx_feed(&ctx, rx_buf[i], s_rx_cb);
        } else if (n == 0) {
            ESP_LOGI(TAG, "MCU %u TCP connection closed", mcu_id);
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "MCU %u recv error: errno=%d", mcu_id, errno);
            break;
        }
    }

    if (s_disconnect_cb)
        s_disconnect_cb(mcu_id);

    /* Deregister. */
    xSemaphoreTake(conn->tx_mutex, portMAX_DELAY);
    conn->fd     = -1;
    conn->active = false;
    xSemaphoreGive(conn->tx_mutex);

    if (atomic_fetch_sub(&s_mcu_count, 1) == 1)
        led_status_set(LED_STATE_WIFI_UP);

    close(fd);
    rx_ctx_free(&ctx);
    ESP_LOGI(TAG, "MCU %u connection task exiting", mcu_id);
    vTaskDelete(NULL);
}

/* ── Listener task ───────────────────────────────────────────────────────── */

static void tcp_listener_task(void *pvParam) {
    (void)pvParam;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(KWM_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, KWM_MAX_MCU) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", KWM_TCP_PORT);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char ip_str[16];
        inet_ntoa_r(client_addr.sin_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "New connection from %s:%d (fd=%d)",
                 ip_str, ntohs(client_addr.sin_port), client_fd);

        conn_task_arg_t *arg = malloc(sizeof(*arg));
        if (!arg) {
            close(client_fd);
            continue;
        }
        arg->fd     = client_fd;
        arg->mcu_id = KWM_MAX_MCU;

        char task_name[24];
        snprintf(task_name, sizeof(task_name), "mcu_conn_%s", ip_str);
        if (xTaskCreate(mcu_conn_task, task_name, 4096, arg, 9, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create MCU conn task");
            free(arg);
            close(client_fd);
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t tcp_server_init(tcp_server_rx_cb_t rx_cb,
                           tcp_server_connect_cb_t connect_cb,
                           tcp_server_disconnect_cb_t disconnect_cb) {
    s_rx_cb         = rx_cb;
    s_connect_cb    = connect_cb;
    s_disconnect_cb = disconnect_cb;

    for (int i = 0; i < KWM_MAX_MCU; i++) {
        s_conns[i].fd       = -1;
        s_conns[i].active   = false;
        s_conns[i].tx_mutex = xSemaphoreCreateMutex();
        if (!s_conns[i].tx_mutex) return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(tcp_listener_task, "tcp_listener", 4096, NULL, 8, NULL) != pdPASS)
        return ESP_FAIL;

    return ESP_OK;
}

bool tcp_server_mcu_connected(uint8_t mcu_id) {
    if (mcu_id >= KWM_MAX_MCU) return false;
    return s_conns[mcu_id].fd >= 0;
}
