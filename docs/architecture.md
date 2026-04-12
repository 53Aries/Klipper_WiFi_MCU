# KlipperESPwifi — System Architecture

## Overview

Wireless Klipper MCU boards using STM32 + ESP32-C5 over WiFi 6, with the Pi 5
hosting a dedicated isolated WLAN via a second ESP32-C5.

## System Diagram

```
┌─────────────────────────────────────────────┐
│           Raspberry Pi 5 (Host)             │
│                                             │
│  Klipper ◄──► wlan0 (via ESP-Hosted)       │
│               192.168.42.1                  │
│  hostapd → WiFi 6 AP, 5GHz, ch.36          │
└──────────────┬──────────────────────────────┘
               │ SPI (ESP-Hosted-FG)
           ESP32-C5 #0  [Pi WiFi adapter]
               │
               │ 802.11ax WiFi 6, 5GHz
               │ SSID: klipper-mcu-net (hidden)
               │ 192.168.42.0/24
       ┌───────┼───────┐
       ▼       ▼       ▼
   C5 #1    C5 #2    C5 #3     [MCU board bridge firmware]
  .42.11   .42.12   .42.13
   UART     UART     UART
  STM32    STM32    STM32      [Stock Klipper MCU firmware]
```

## Klipper Config (printer.cfg)

```ini
[mcu toolhead]
serial: socket://192.168.42.11:23
restart_method: command

[mcu aux]
serial: socket://192.168.42.12:23
restart_method: command
```

## Component Roles

| Component | Firmware | Job |
|-----------|----------|-----|
| Pi-side ESP32-C5 | ESP-Hosted-FG | Appears as wlan0 to Linux; runs as AP via hostapd |
| MCU-side ESP32-C5 | esp-bridge (this repo) | UART↔TCP bridge; connects to Pi AP as station |
| STM32 | Klipper MCU firmware (unmodified) | Stepper/heater/sensor control |
| Pi 5 | Klipper host + hostapd + dnsmasq | Move planning, G-code, AP |

## Network

- Pi AP IP: `192.168.42.1`
- MCU boards: `192.168.42.11–.13` (static, MAC-reserved via dnsmasq)
- Closed WLAN — no upstream routing, no home network dependency
- Hidden SSID, WPA2

## Why This Works

Neither Klipper on the Pi nor Klipper on the STM32 knows anything about WiFi.
From Klipper's perspective it's a socket connection. From the STM32's perspective
it's a UART. The two C5s handle everything in between.
