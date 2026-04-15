# SPI Pinout — Pi 5 ↔ ESP32-C5 DevKit V2.0 (esp-hosted-pi)

The pi-side ESP32-C5 DevKit acts as an SPI slave running ESP-Hosted firmware.
The Pi is the SPI master. Six signals are required: the four standard SPI lines
plus two ESP-Hosted control signals (Handshake and DataReady), and a reset line.

---

## Wiring Table

Signals are ordered by physical pin number. All wires land on the **left (odd) column** of the 40-pin header except 5V power (pin 4), Handshake (pin 18), and SPI CS (pin 24), which are on the right column.

All DevKit connections use **edge connector pins** — no bottom-pad soldering.

| Signal      | Pi 5 BCM | Pi 5 Physical Pin | Side  | DevKit GPIO | Direction  |
|-------------|----------|-------------------|-------|-------------|------------|
| 5V          | —        | Pin 4             | Right | 5V          | Pi → ESP   |
| GND         | GND      | Pin 9             | Left  | GND         | —          |
| Reset       | BCM 17   | Pin 11            | Left  | RST / EN    | Pi → ESP   |
| DataReady   | BCM 27   | Pin 13            | Left  | GPIO 4      | ESP → Pi   |
| Handshake   | BCM 24   | Pin 18            | Right | GPIO 3      | ESP → Pi   |
| SPI MOSI    | BCM 10   | Pin 19            | Left  | GPIO 7      | Pi → ESP   |
| SPI MISO    | BCM 9    | Pin 21            | Left  | GPIO 2      | ESP → Pi   |
| SPI SCLK    | BCM 11   | Pin 23            | Left  | GPIO 6      | Pi → ESP   |
| SPI CS      | BCM 8    | Pin 24            | Right | GPIO 10     | Pi → ESP   |

> **Power note:** The devkit's 3V3 pin is regulator output only. Connect Pi 5V (pin 4) → DevKit 5V. The onboard regulator steps it down to 3.3V internally.

> ⚠ **Pin 15 (BCM22) and Pin 16 (BCM23) cannot be used for Handshake on Pi 5.**
> - BCM22 is permanently claimed by the `spi10` controller as `cs-gpios` CS0 (`2712_BOOT_CS_N`) — requesting it causes `EBUSY`.
> - BCM23 has a hardware pull-up (`2712_BOOT_MISO`, used in the Pi 5 boot SPI chain) that holds the line HIGH permanently. The driver's rising-edge interrupt never fires because the line never goes low.
>
> We use **Pin 18 (BCM24)** — a plain GPIO with no boot function.

---

## Linux Driver Parameters (Pi 5 / RP1)

Pi 5 GPIOs are addressed as `BCM + RP1_BASE`. The base offset varies by OS version:
- **Bookworm**: RP1_BASE = 512
- **Trixie**: RP1_BASE = 569

The `setup-esp-hosted.sh` script **auto-detects** the base by scanning `/sys/class/gpio/` for the `pinctrl-rp1` chip. Example with Trixie (base 569):

```
resetpin=586        # BCM 17 + 569  (Pin 11)
spi-handshake=593   # BCM 24 + 569  (Pin 18)
spi-dataready=596   # BCM 27 + 569  (Pin 13)
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

## ⚠ JTAG Note

GPIO2, GPIO3, and GPIO4 are shared with the ESP32-C5’s JTAG interface
(MTMS, MTDI, MTCK respectively). JTAG debugging is **not available**
while the SPI link is in use.

---

## SPI Mode

**Mode 3** (CPOL=1, CPHA=1) — set by `ESP_SPI_PRIV_MODE_3` (Kconfig default for C5).
The Pi's `spidev` driver must be configured for mode 3 to match.
