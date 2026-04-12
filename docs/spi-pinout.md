# SPI Pinout — Pi 5 ↔ XIAO ESP32-C5 (esp-hosted-pi)

The Pi-side XIAO acts as an SPI slave running ESP-Hosted firmware.
The Pi is the SPI master. Six signals are required: the four standard SPI lines
plus two ESP-Hosted control signals (Handshake and DataReady), and a reset line.

---

## Wiring Table

| Signal      | Pi 5 BCM | Pi 5 Physical Pin | XIAO GPIO | XIAO Pad | Direction      |
|-------------|----------|-------------------|-----------|----------|----------------|
| SPI MOSI    | BCM 10   | Pin 19            | GPIO 7    | D3       | Pi → XIAO      |
| SPI MISO    | BCM 9    | Pin 21            | GPIO 2    | MTMS     | XIAO → Pi      |
| SPI SCLK    | BCM 11   | Pin 23            | GPIO 6    | ADC\_BAT | Pi → XIAO      |
| SPI CS      | BCM 8    | Pin 24            | GPIO 10   | D10/MOSI | Pi → XIAO      |
| Handshake   | BCM 22   | Pin 15            | GPIO 3    | MTDI     | XIAO → Pi      |
| DataReady   | BCM 27   | Pin 13            | GPIO 4    | MTCK     | XIAO → Pi      |
| Reset       | BCM 6    | Pin 31            | RST / EN  | RESET    | Pi → XIAO      |
| GND         | GND      | Pin 6 / Pin 9     | GND       | GND      | —              |
| 3.3V        | 3V3      | Pin 1 / Pin 17    | 3V3       | 3V3      | Pi → XIAO      |

---

## Linux Driver Parameters (Pi 5 / RP1)

Pi 5 GPIOs are addressed as `BCM + 512` due to the RP1 I/O controller offset:

```
resetpin=518        # BCM 6  + 512  (Pin 31)
spi-handshake=534   # BCM 22 + 512  (Pin 15)
spi-dataready=539   # BCM 27 + 512  (Pin 13)
```

SPI bus: `/dev/spidev0.0` (SPI0, CE0)

---

## ESP-Hosted Firmware Config Source

These are the **Kconfig defaults** for `IDF_TARGET_ESP32C5` in
`main/Kconfig.projbuild`. They are **not** overridden in
`sdkconfig.defaults.esp32c5` — the defaults match the wiring.

To change a pin, add the relevant `CONFIG_ESP_SPI_HSPI_GPIO_*` line to
`sdkconfig.defaults.esp32c5`.

---

## ⚠ JTAG Conflict Note

GPIO2, GPIO3, and GPIO4 are shared with the XIAO's JTAG interface
(MTMS, MTDI, MTCK respectively). JTAG debugging is **not available**
while the SPI link is in use. GPIO6 is also the battery ADC enable pin —
do not use the battery voltage measurement feature in this configuration.

---

## SPI Mode

**Mode 3** (CPOL=1, CPHA=1) — set by `ESP_SPI_PRIV_MODE_3` (Kconfig default for C5).
The Pi's `spidev` driver must be configured for mode 3 to match.
