#include "bridge.h"
#include "wifi.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#define TAG        "bridge"
#define RX_BUF     512
#define RETRY_MS   500

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL,
                                        ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static int tcp_connect(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(KLIPPER_PORT),
    };
    inet_pton(AF_INET, KLIPPER_HOST_IP, &addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    // Kill Nagle — critical for latency
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "TCP connected to %s:%d", KLIPPER_HOST_IP, KLIPPER_PORT);
    return sock;
}

// UART → TCP
static void uart_to_tcp_task(void *arg)
{
    int sock = *(int *)arg;
    uint8_t buf[RX_BUF];

    while (1) {
        int len = uart_read_bytes(UART_NUM, buf, sizeof(buf),
                                  pdMS_TO_TICKS(10));
        if (len > 0) {
            if (send(sock, buf, len, 0) < 0) {
                ESP_LOGW(TAG, "TCP send error");
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

// TCP → UART
static void tcp_to_uart_task(void *arg)
{
    int sock = *(int *)arg;
    uint8_t buf[RX_BUF];

    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len <= 0) {
            ESP_LOGW(TAG, "TCP recv error or closed (%d)", len);
            break;
        }
        uart_write_bytes(UART_NUM, buf, len);
    }
    vTaskDelete(NULL);
}

void bridge_task(void *arg)
{
    uart_init();

    // Subscribe this task to the watchdog initialised in main.
    // We must call esp_task_wdt_reset() regularly or the chip resets.
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        // Feed watchdog — we're alive and looping
        esp_task_wdt_reset();

        // Wait for WiFi before attempting TCP
        wifi_wait_connected();

        int sock = tcp_connect();
        if (sock < 0) {
            ESP_LOGW(TAG, "TCP connect failed — retrying in %dms", RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }

        // Spawn bidirectional pipe tasks pinned to core 1.
        // Core 0 handles WiFi/lwIP; core 1 is dedicated to the bridge —
        // eliminates FreeRTOS scheduler contention with the TCP/IP stack.
        TaskHandle_t t1, t2;
        xTaskCreatePinnedToCore(uart_to_tcp_task, "u2t", 4096, &sock, 10, &t1, 1);
        xTaskCreatePinnedToCore(tcp_to_uart_task, "t2u", 4096, &sock, 10, &t2, 1);

        // Wait for either task to finish (means socket died).
        // Feed watchdog each iteration — bridge is healthy while data flows.
        while (eTaskGetState(t1) != eDeleted && eTaskGetState(t2) != eDeleted) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGW(TAG, "Bridge died — cleaning up");
        // Only delete the task that is still alive — the other already called
        // vTaskDelete(NULL) on itself, making its handle stale.
        if (eTaskGetState(t1) != eDeleted) vTaskDelete(t1);
        if (eTaskGetState(t2) != eDeleted) vTaskDelete(t2);
        close(sock);

        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
}
