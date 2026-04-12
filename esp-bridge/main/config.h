#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────
#define WIFI_SSID        "klipper-mcu-net"
#define WIFI_PASSWORD    "CHANGE_ME"
#define WIFI_CHANNEL     36           // Must match hostapd.conf

// Static IP — must match dnsmasq static lease for this board's MAC
#define STATIC_IP        "192.168.42.11"
#define STATIC_GATEWAY   "192.168.42.1"
#define STATIC_NETMASK   "255.255.255.0"

// Pi AP BSSID — fill in after Pi is set up (speeds up reconnect)
// Leave all zeros to disable pinning during development
#define AP_BSSID         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define AP_BSSID_SET     false

// ── TCP ───────────────────────────────────────────────────────────────────
#define KLIPPER_HOST_IP  "192.168.42.1"   // Pi — Klipper listens here
#define KLIPPER_PORT     23               // Klipper socket:// port

// ── UART ──────────────────────────────────────────────────────────────────
#define UART_NUM         UART_NUM_1
#define UART_TX_PIN      6
#define UART_RX_PIN      7
#define UART_BAUD        1000000          // 1Mbaud
#define UART_BUF_SIZE    1024

// ── STM32 control ─────────────────────────────────────────────────────────
// Wired to STM32 NRST and BOOT0 respectively.
// NRST: pull low briefly to hard-reset the STM32.
// BOOT0: pull high before reset to enter DFU mode for OTA flashing.
#define STM32_NRST_PIN   4
#define STM32_BOOT0_PIN  5
