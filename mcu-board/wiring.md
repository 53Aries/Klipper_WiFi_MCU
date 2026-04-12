# MCU Board Wiring — Any MCU + ESP32-C5

The MCU sees nothing but a UART connection. It has no knowledge of the ESP32
or WiFi — the bridge is fully transparent. Any MCU running Klipper over UART
works here regardless of vendor or family.

## MCU ↔ ESP32-C5 (UART Bridge)

| STM32 Pin | Direction | ESP32-C5 Pin | Purpose |
|-----------|-----------|--------------|---------|
| PA9 (USART1 TX) | → | GPIO6 (RX) | Data TX |
| PA10 (USART1 RX) | ← | GPIO7 (TX) | Data RX |
| NRST | ← | GPIO4 | STM32 reset (C5 pulls low to reset) |
| BOOT0 | ← | GPIO5 | DFU mode trigger for OTA flashing |
| 3.3V | — | 3.3V | Shared power |
| GND | — | GND | Common ground |

## Notes

- Use hardware USART with DMA (USART1/2/3 on STM32) — not bit-bang
- UART baud: 1,000,000 (1Mbaud). Configure in Klipper and config.h
- NRST: C5 GPIO4 pulled low briefly to hard-reset STM32
- BOOT0: C5 GPIO5 high + NRST toggle puts STM32 in DFU for flashing
- Decouple C5 3.3V rail with 10µF + 100nF caps — WiFi TX draws 200-300mA peaks
- Keep WiFi antenna away from stepper drivers and switching supplies

## Power

- C5 can be powered from the STM32 board's 3.3V rail if it can supply 500mA+
- Otherwise use a dedicated LDO for the C5

## Klipper MCU Firmware Build

When building Klipper for the MCU, the only relevant setting is:
- Communication interface: USART/Serial (not USB)

Which specific UART pins to use depends on the MCU board — pick any hardware
USART with DMA support and wire it to the C5's GPIO6/7.
