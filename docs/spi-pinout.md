# SPI Pinout — Pi 5 ↔ XIAO ESP32-C5 (esp-hosted-pi)

The Pi-side XIAO acts as an SPI slave running ESP-Hosted firmware.
The Pi is the SPI master. Six signals are required: the four standard SPI lines
plus two ESP-Hosted control signals (Handshake and DataReady), and a reset line.

---

## Wiring Table

Signals are ordered by physical pin number. All wires land on the **left (odd) column** of the 40-pin header except SPI CS (pin 24) and 5V power (pin 4), both on the right column.

| Signal      | Pi 5 BCM | Pi 5 Physical Pin | Side  | XIAO GPIO | XIAO Pad | Direction  |
|-------------|----------|-------------------|-------|-----------|----------|------------|
| 5V          | —        | Pin 4             | Right | —         | VBUS     | Pi → XIAO  |
| GND         | GND      | Pin 9             | Left  | GND       | GND      | —          |
| Reset       | BCM 17   | Pin 11            | Left  | RST / EN  | RESET    | Pi → XIAO  |
| DataReady   | BCM 27   | Pin 13            | Left  | GPIO 4    | MTCK     | XIAO → Pi  |
| Handshake   | BCM 23   | Pin 16            | Left  | GPIO 3    | MTDI     | XIAO → Pi  |
| SPI MOSI    | BCM 10   | Pin 19            | Left  | GPIO 7    | D3       | Pi → XIAO  |
| SPI MISO    | BCM 9    | Pin 21            | Left  | GPIO 2    | MTMS     | XIAO → Pi  |
| SPI SCLK    | BCM 11   | Pin 23            | Left  | GPIO 6    | ADC\_BAT | Pi → XIAO  |
| SPI CS      | BCM 8    | Pin 24 ⚠          | Right | GPIO 10   | D10/MOSI | Pi → XIAO  |

> **Power note:** The XIAO's 3V3 pin is regulator output only. Connect Pi 5V (pin 4) → XIAO VBUS. The onboard regulator steps it down to 3.3V internally.

> ⚠ **Pin 24 (CS) is the only SPI wire on the right side.** We use SPI0 CE0 (GPIO 8, pin 24). The spidev driver is prevented from claiming CE0 by a boot-time overlay (`spidev-disabler`) installed by the setup script.

> ⚠ **Pin 15 (BCM22) cannot be used for Handshake on Pi 5.** BCM22 is permanently claimed by the `spi10` controller as its `cs-gpios` CS0 entry in the Pi 5 base device tree (`2712_BOOT_CS_N`). Attempting to use it for Handshake causes `EBUSY` when `esp32_spi` tries to request it. We use Pin 16 (BCM23) instead.

---

## Linux Driver Parameters (Pi 5 / RP1)

Pi 5 GPIOs are addressed as `BCM + 512` due to the RP1 I/O controller offset:

```
resetpin=529        # BCM 17 + 512  (Pin 11)
spi-handshake=535   # BCM 23 + 512  (Pin 16)
spi-dataready=539   # BCM 27 + 512  (Pin 13)
```

SPI bus: SPI0, CE0 (`spi_bus=10 spi_cs=0` — CE0 is freed at boot by the `spidev-disabler` overlay)

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
