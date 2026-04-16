# KlipperESPwifi — System Architecture

## Overview

Wireless Klipper MCU boards using STM32 + ESP32-C5 over WiFi 6, with the Pi 5
connected to a dedicated ESP32-C5 WiFi 6 AP via USB (CDC-NCM).

## System Diagram

```
┌─────────────────────────────────────────────┐
│           Raspberry Pi 5 (Host)             │
│                                             │
│  Klipper ◄──► usb0 (USB CDC-NCM ethernet)  │
│               192.168.42.1                  │
└──────────────┬──────────────────────────────┘
               │ USB (CDC-NCM, no driver needed)
           ESP32-C5 #0  [Pi WiFi adapter]
               │ USB OTG → appears as usb0/eth1 on Pi
               │ softAP: 802.11ax WiFi 6, 5GHz, OFDMA
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
| Pi-side ESP32-C5 | esp-hosted-pi (TinyUSB CDC-NCM + softAP) | USB Ethernet to Pi; WiFi 6 AP for MCU boards |
| MCU-side ESP32-C5 | esp-bridge (this repo) | UART↔TCP bridge; connects to Pi C5 AP as station |
| STM32 | Klipper MCU firmware (unmodified) | Stepper/heater/sensor control |
| Pi 5 | Klipper host | Move planning, G-code |

## Network

- Pi AP IP: `192.168.42.1`
- MCU boards: `192.168.42.11–.13` (static, MAC-reserved via dnsmasq)
- Closed WLAN — no upstream routing, no home network dependency
- Hidden SSID, WPA2

## Why This Works

Neither Klipper on the Pi nor Klipper on the STM32 knows anything about WiFi.
From Klipper's perspective it's a socket connection on usb0. From the STM32's
perspective it's a UART. The two C5s handle everything in between.

The Pi-side C5 connects via a single USB cable — the Pi sees it as a standard
USB Ethernet adapter using the built-in `cdc_ncm` kernel module. No custom
drivers, no SPI wiring. The C5's internal WiFi 6 OFDMA scheduler handles
simultaneous traffic from multiple MCU boards efficiently.
