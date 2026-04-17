# Klipper WiFi MCU — Setup & Operation Guide

Replaces USB serial between a Klipper host (Raspberry Pi 5) and one or more
STM32-based printer MCUs with a WiFi 6 link.  Each MCU appears to Klipper as
a normal serial device (`/dev/kwm0`, `/dev/kwm1`, …).

---

## Table of Contents

1. [System Architecture](#1-system-architecture)
2. [Hardware Required](#2-hardware-required)
3. [Wiring](#3-wiring)
4. [Software Prerequisites](#4-software-prerequisites)
5. [Build and Flash — Host ESP32-C5](#5-build-and-flash--host-esp32-c5)
6. [Build and Flash — MCU ESP32-C5](#6-build-and-flash--mcu-esp32-c5)
7. [Pi5 Setup](#7-pi5-setup)
8. [Running the Bridge Daemon](#8-running-the-bridge-daemon)
9. [Klipper Configuration](#9-klipper-configuration)
10. [Verifying Operation](#10-verifying-operation)
11. [Systemd Service (Auto-start)](#11-systemd-service-auto-start)
12. [Protocol Reference](#12-protocol-reference)
13. [Troubleshooting](#13-troubleshooting)
14. [Adding More MCUs](#14-adding-more-mcus)

---

## 1. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Raspberry Pi 5                                                  │
│                                                                  │
│  Klipper (klippy.py)                                             │
│       │  /dev/kwm0   /dev/kwm1  …  PTY symlinks                 │
│  klipper_bridge.py  ←──── spi_driver.py                         │
│       │                        │                                 │
│  GPIO8 (DATA_READY ←)     SPI0 (MOSI/MISO/SCLK/CE0)            │
│  GPIO7 (HANDSHAKE  →)                                            │
└──────────────────────────────┬──────────────────────────────────┘
                               │  256-byte SPI frames (10 MHz)
                               │  Full-duplex, SPI mode 3
                    ┌──────────┴──────────┐
                    │  Host ESP32-C5       │  compact "Super Mini"
                    │  WiFi 6 soft-AP      │  4 MB flash
                    │  TCP server :8842    │  192.168.42.1
                    └──────────┬──────────┘
                               │  WiFi 6  (802.11ax, OFDMA)
                               │  hidden SSID "KlipperMesh"
              ┌────────────────┼────────────────┐
              │                │                │
   ┌──────────┴──────┐ ┌───────┴───────┐  (up to 8 MCU ESPs)
   │ MCU ESP32-C5 #0 │ │ MCU ESP32-C5 │
   │ WiFi 6 STA      │ │ WiFi 6 STA   │
   │ TCP client      │ │ TCP client   │
   └──────────┬──────┘ └───────┬───────┘
              │ UART1 250000   │ UART1 250000
              │ 8N1            │ 8N1
   ┌──────────┴──────┐ ┌───────┴───────┐
   │  STM32 MCU #0   │ │  STM32 MCU   │
   │  (Klipper)      │ │  (Klipper)   │
   └─────────────────┘ └──────────────┘
```

### Data path (Pi → MCU)

1. Klipper writes serial bytes to `/dev/kwm0` (PTY slave).
2. `klipper_bridge.py` reads from the PTY master, packs a 256-byte SPI frame.
3. Pi5 pushes the frame to the Host ESP32-C5 over SPI.
4. Host ESP routes the payload over TCP to the matching MCU ESP connection.
5. MCU ESP writes the bytes out UART1 to the STM32.

### Data path (MCU → Pi)

1. STM32 sends bytes over UART to MCU ESP.
2. MCU ESP wraps them in a TCP frame and sends to Host ESP.
3. Host ESP builds a 256-byte SPI frame and asserts DATA_READY (GPIO25 → Pi GPIO8).
4. Pi5 receives the interrupt, performs an SPI transfer, unpacks the frame.
5. `klipper_bridge.py` writes the payload to the correct PTY master.
6. Klipper reads the bytes from the PTY slave `/dev/kwm0`.

---

## 2. Hardware Required

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | **Raspberry Pi 5** (2GB+ RAM) | Host computer running Klipper |
| 1 | **ESP32-C5 compact board** ("Super Mini") | Host ESP — SPI to Pi, WiFi AP |
| 1+ | **Seeed Studio XIAO ESP32C5** | MCU ESP — one per Klipper MCU |
| 1+ | **STM32 Klipper MCU board** | e.g. SKR, Octopus, etc. |
| — | Dupont/JST wires | SPI + GPIO from Pi to Host ESP |
| — | UART wires (2) | TX/RX from MCU ESP to STM32 |

> **Up to 8 MCU-side ESPs** are supported simultaneously via WiFi 6 OFDMA.

---

## 3. Wiring

### Pi5 ↔ Host ESP32-C5 (SPI + GPIO)

```
Pi 5 Header Pin   BCM GPIO   ESP32-C5 Compact Pin   Signal
──────────────────────────────────────────────────────────
Pin 19            MOSI       GPIO7  (FSPID)          SPI data Pi→ESP
Pin 21            MISO       GPIO2  (FSPIQ)          SPI data ESP→Pi
Pin 23            SCLK       GPIO6  (FSPICLK)        SPI clock
Pin 24            CE0        GPIO10 (FSPICS0)        SPI chip-select
Pin 8   (BCM 8)   GPIO8      GPIO25                  DATA_READY (ESP→Pi)
Pin 26  (BCM 7)   GPIO7      GPIO26                  HANDSHAKE  (Pi→ESP)
Pin 6             GND        GND                     Common ground
Pin 1             3.3 V      3.3 V                   Power (if not USB)
```

> The Host ESP is powered via its USB connector for flashing and can remain
> USB-powered in use, or powered from the Pi's 3.3 V rail if current allows
> (the ESP32-C5 idle draw is ~20 mA, ~120 mA peak Tx).

### MCU ESP32-C5 (XIAO) ↔ STM32 (UART)

```
XIAO ESP32C5 Pin   STM32 Pin      Signal
─────────────────────────────────────────
D6 / GPIO11 (TX) →  UART RX pin   Serial data ESP→STM32
D7 / GPIO12 (RX) ←  UART TX pin   Serial data STM32→ESP
GND              —  GND           Common ground
```

> The MCU ESP is powered via USB. It does not need a wired connection to the
> Pi — only to the STM32 UART and a USB power source.
>
> Baud rate: **250000** (standard Klipper serial rate; hardcoded in firmware).

---

## 4. Software Prerequisites

### On the build machine (Windows/Linux/macOS)

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html):

```powershell
# Windows PowerShell
pip install platformio
```

Install the pioarduino platform (provides IDF 5.5 + RISC-V toolchain with
ESP32-C5 support — one-time, ~700 MB download):

```powershell
pio pkg install -g -p "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip"
```

### On the Pi5

See [section 7](#7-pi5-setup) for the full Pi5 configuration walkthrough
(system update, package install, SPI enable, permissions, repo clone).

---

## 5. Build and Flash — Host ESP32-C5

The host firmware runs on the compact ESP32-C5 board connected to the Pi via SPI.

### Build

```powershell
# Windows — strip MinGW vars so idf_tools.py doesn't refuse to run
$env_clean = @{MSYSTEM=$null; MINGW_PREFIX=$null; MINGW_CHOST=$null}
$env_clean.Keys | ForEach-Object { [Environment]::SetEnvironmentVariable($_, '', 'Process') }

pio run -e host_esp
```

On Linux/macOS:

```bash
pio run -e host_esp
```

The firmware binary is at:

```
.pio/build/host_esp/firmware.factory.bin
```

### Flash

1. Connect the Host ESP32-C5 to your PC via USB (use the **bottom** USB
   connector on the compact board — this is the USB Serial/JTAG port).
2. Hold the **BOOT** button while pressing **RESET**, then release BOOT.
   (Or just plug in with BOOT held for boards that auto-enter bootloader.)

```powershell
pio run -e host_esp -t upload
```

On Linux, if you get a permission error on `/dev/ttyUSBx`:

```bash
sudo usermod -aG dialout $USER   # log out and back in
pio run -e host_esp -t upload
```

### Verify the host ESP is running

Monitor the serial output (115200 baud):

```powershell
pio device monitor -e host_esp
```

Expected output on boot:

```
I (xxx) main: === Klipper WiFi MCU - Host ESP32-C5 ===
I (xxx) main: IDF 5.5.4 | cores=1 | flash=4MB
I (xxx) main: Protocol: SPI frame 256 bytes | TCP port 8842 | max MCUs 8
I (xxx) wifi_ap: WiFi 6 AP started: SSID=KlipperMesh (hidden) channel=6 IP=192.168.42.1
I (xxx) tcp_server: TCP server listening on port 8842
I (xxx) bridge: Bridge initialised
```

---

## 6. Build and Flash — MCU ESP32-C5 (XIAO)

**All MCU-side boards use the same firmware binary.** No per-device
configuration is needed before flashing.  Each board derives its own MCU ID
(0–7) at runtime from its hardware MAC address.

### Build

```powershell
pio run -e mcu_esp
```

Binary at:

```
.pio/build/mcu_esp/firmware.factory.bin
```

### Flash (repeat for every XIAO ESP32C5)

Connect the XIAO ESP32C5 to your PC via USB-C.

```powershell
pio run -e mcu_esp -t upload
```

### Verify each MCU ESP is running

```powershell
pio device monitor -e mcu_esp
```

Expected output on boot:

```
I (xxx) main: === Klipper WiFi MCU - MCU Bridge ESP32-C5 ===
I (xxx) main: IDF 5.5.4
I (xxx) main: MAC  ac:15:18:3a:b2:7f
I (xxx) main: MCU ID = 3  (hash of MAC[3:5] mod 8)
I (xxx) main: Pi PTY will appear as /dev/kwm3
I (xxx) wifi_sta: Connected to AP 'KlipperMesh'
I (xxx) tcp_client: TCP connected to host (fd=4)
I (xxx) tcp_client: Sent CONNECT: mcu_id=3  MAC=ac:15:18:3a:b2:7f
```

The MCU ID (3 in this example) is **stable across reboots** — the same board
always gets the same ID.  Record the MAC → ID mapping for each board so you
know which PTY to reference in `printer.cfg`.

### If two boards collide on the same ID

The FNV hash of the MAC is spread across 8 slots; collision probability is
low for small fleets (≈12 % chance with 2 boards, ≈26 % with 3).  If a
collision occurs both boards will log the same ID and one will kick the other
off the host.  Resolution: use the NVS shell on one board to override its ID:

```
idf.py monitor   # or pio device monitor
# at the esp> prompt:
nvs_set kwm mcu_id u8 5
```

*(NVS override support is planned for a future firmware revision.)*

---

## 7. Pi5 Setup

Work through these steps in order on a fresh Pi OS Lite install.

### Step 1 — Update the system

```bash
sudo apt update && sudo apt upgrade -y
sudo reboot
```

### Step 2 — Install dependencies

```bash
sudo apt install -y python3-spidev python3-libgpiod git
```

> The GPIO Python bindings package is named `python3-libgpiod` on Pi OS
> Bookworm and Trixie (not `python3-gpiod`).  Both install the same
> `gpiod` Python module.

Verify Python 3 is available (Pi OS Lite ships with it, but worth confirming):

```bash
python3 --version
# Python 3.11.x or newer
```

### Step 3 — Enable the SPI interface

```bash
sudo raspi-config
# Interface Options → SPI → Enable → Finish → Yes (reboot when prompted)
```

Or non-interactively:

```bash
sudo raspi-config nonint do_spi 0
sudo reboot
```

After reboot, verify the SPI device nodes appeared:

```bash
ls /dev/spidev0.*
# Expected: /dev/spidev0.0  /dev/spidev0.1
```

If the files are missing, SPI was not enabled.  Check
`/boot/firmware/config.txt` and confirm it contains `dtparam=spi=on`.

### Step 4 — Permissions

#### SPI

`/dev/spidev0.0` is owned by `root:spi` (mode 660).  The bridge daemon runs
as root via systemd (section 11), so no change is needed for normal
operation.

For running the daemon **manually without `sudo`** (useful during testing),
add your user to the `spi` group:

```bash
sudo usermod -aG spi $USER
```

#### GPIO

`/dev/gpiochip4` is owned by `root:gpio` (mode 660).  Same situation — root
is fine for the service, but add yourself to `gpio` for manual testing:

```bash
sudo usermod -aG gpio $USER
```

Apply both group changes in one go and re-login:

```bash
sudo usermod -aG spi,gpio $USER
# Log out and back in, then verify:
groups   # should include spi and gpio
```

### Step 5 — Verify GPIO chip

Pi 5 uses `/dev/gpiochip4` (older Pi models use gpiochip0).  Confirm it
exists:

```bash
ls /dev/gpiochip*
# Should include /dev/gpiochip4
```

The bridge daemon defaults to `/dev/gpiochip4` — no configuration needed
unless you are running on a non-Pi 5 board.

### Step 6 — Clone the repository

```bash
cd ~
git clone https://github.com/53Aries/Klipper_WiFi_MCU.git
cd Klipper_WiFi_MCU
```

---

## 8. Running the Bridge Daemon

The bridge daemon (`pi_host/klipper_bridge.py`) must be running before
Klipper starts.  It creates the `/dev/kwmN` symlinks that Klipper references.

### Basic usage

```bash
sudo python3 pi_host/klipper_bridge.py --mcus 0 1
```

`--mcus` lists the MCU IDs you expect to connect.  It must include every ID
referenced in `printer.cfg`.  The daemon creates the PTY and symlink for each
listed ID at startup — Klipper can open the PTY even before the MCU ESP
physically connects.

### All options

```
--mcus ID [ID ...]     MCU IDs to bridge (default: 0)
--spi-bus N            SPI bus number (default: 0)
--spi-dev N            SPI device/CE number (default: 0)
--spi-speed HZ         SPI clock in Hz (default: 10000000 = 10 MHz)
--gpio-chip PATH       GPIO chip device (default: /dev/gpiochip4)
--pin-dr BCM           BCM GPIO for DATA_READY input (default: 8)
--pin-hs BCM           BCM GPIO for HANDSHAKE output (default: 7)
--verbose / -v         Enable debug logging
```

### Example: 3 MCUs, verbose

```bash
sudo python3 pi_host/klipper_bridge.py --mcus 0 1 3 -v
```

### Expected startup output

```
2026-04-16 12:00:01 klipper_bridge INFO SPI opened: bus=0 dev=0 speed=10000000
2026-04-16 12:00:01 klipper_bridge INFO MCU 0 PTY: /dev/pts/2
2026-04-16 12:00:01 klipper_bridge INFO MCU 0 symlink: /dev/kwm0 -> /dev/pts/2
2026-04-16 12:00:01 klipper_bridge INFO MCU 1 PTY: /dev/pts/3
2026-04-16 12:00:01 klipper_bridge INFO MCU 1 symlink: /dev/kwm1 -> /dev/pts/3
2026-04-16 12:00:01 klipper_bridge INFO Bridge running. Ctrl-C to stop.
```

When an MCU ESP connects over WiFi, the host ESP logs (visible on the host
serial monitor):

```
I tcp_server: MCU 3 connected  MAC=ac:15:18:3a:b2:7f  → /dev/kwm3
```

---

## 9. Klipper Configuration

Edit `~/printer_data/config/printer.cfg` (or wherever your config lives).

### Single additional MCU

```ini
[mcu secondary]
serial: /dev/kwm0
```

### Multiple MCUs

Each MCU needs its own `[mcu name]` section.  Use the ID reported by the MCU
ESP at boot (visible in its serial monitor output).

```ini
[mcu mcu0]
serial: /dev/kwm0

[mcu mcu1]
serial: /dev/kwm1

[mcu mcu3]
serial: /dev/kwm3
```

Then reference MCU-specific pins as usual:

```ini
[stepper_x]
step_pin: mcu1:PA1
dir_pin:  mcu1:PA2
...
```

### Notes

- The bridge daemon must be running and the symlinks must exist **before**
  Klipper attempts to connect.  Use the systemd service described in section 11
  to ensure correct start order.
- Klipper reconnects automatically if the serial link drops (e.g. MCU ESP
  reboots and reconnects via WiFi).  The PTY stays open on the Pi side so
  Klipper sees a continuous device; only the underlying WiFi/TCP path
  reconnects behind the scenes.

---

## 10. Verifying Operation

### Check bridge daemon logs

```bash
sudo journalctl -u kwm-bridge -f   # if running as service
# or watch the terminal where you started the daemon
```

### Check host ESP serial monitor

Connect the Host ESP via USB and open a monitor while the system is running:

```powershell
pio device monitor -e host_esp
```

You should see per-MCU connection events and a periodic status line:

```
I main: Heap free: 284312 bytes | SPI rx pending: 0 | WiFi STAs: 2
I tcp_server: MCU 0 connected  MAC=ac:15:18:aa:bb:cc  → /dev/kwm0
I tcp_server: MCU 3 connected  MAC=ac:15:18:3a:b2:7f  → /dev/kwm3
```

### Check MCU ESP serial monitor (one at a time)

```powershell
pio device monitor -e mcu_esp
```

Normal running state:

```
I tcp_client: TCP connected to host (fd=4)
I tcp_client: Sent CONNECT: mcu_id=0  MAC=ac:15:18:aa:bb:cc
I kwm_uart: UART RX task started (port=1 baud=250000 TX=16 RX=17)
```

### Confirm PTY symlinks exist on Pi

```bash
ls -la /dev/kwm*
# lrwxrwxrwx 1 root root 10 Apr 16 12:00 /dev/kwm0 -> /dev/pts/2
# lrwxrwxrwx 1 root root 10 Apr 16 12:00 /dev/kwm3 -> /dev/pts/5
```

### Send a test byte from the Pi

```bash
python3 - <<'EOF'
from pi_host.spi_driver import SpiDriver
d = SpiDriver()
d.open()
d.send(mcu_id=0, data=b'\x01\x02\x03')
print("sent")
d.close()
EOF
```

---

## 11. Systemd Service (Auto-start)

Create `/etc/systemd/system/kwm-bridge.service`:

```ini
[Unit]
Description=Klipper WiFi MCU SPI bridge daemon
After=network.target
# Ensure bridge is up before Klipper tries to open serial ports
Before=klipper.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /home/pi/Klipper_WiFi_MCU/pi_host/klipper_bridge.py --mcus 0 1
Restart=always
RestartSec=3
User=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

> Replace `/home/pi/Klipper_WiFi_MCU` with your actual clone path.
> Replace `--mcus 0 1` with the IDs of your actual boards.

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable kwm-bridge
sudo systemctl start kwm-bridge
sudo systemctl status kwm-bridge
```

### Make Klipper wait for the bridge

Edit `/etc/systemd/system/klipper.service` (or the moonraker-managed equivalent)
and add to the `[Unit]` section:

```ini
After=kwm-bridge.service
Requires=kwm-bridge.service
```

---

## 12. Protocol Reference

### WiFi network

| Parameter | Value |
|-----------|-------|
| SSID | `KlipperMesh` (hidden) |
| Password | `klipper42!` |
| Security | WPA3-SAE (PMF required) |
| Band | 2.4 GHz, channel 6 |
| Standard | 802.11ax (WiFi 6) with OFDMA |
| AP IP | `192.168.42.1` |
| DHCP range | `192.168.42.x` (assigned to MCU ESPs) |
| TCP port | `8842` |

### SPI frame (Pi ↔ Host ESP)

Fixed 256-byte full-duplex; both sides transmit simultaneously every transfer.

```
Offset  Size  Field
──────────────────────────────────────────────────────────
0       2     Magic: 0xAB 0xCD
2       1     Command (see below)
3       1     MCU ID [3:0] | Flags [7:4]
4       1     Sequence number (0–255, wraps)
5       1     Reserved (0)
6       2     Payload length, big-endian (0–246)
8       246   Payload (zero-padded)
254     2     CRC16-CCITT of bytes [0..253], big-endian
```

### TCP frame (Host ESP ↔ MCU ESP)

Variable length; stream-framed with magic + CRC.

```
Offset  Size  Field
──────────────────────────────────────────────────────────
0       2     Magic: 0xAB 0xCD
2       1     Command
3       1     MCU ID [3:0] | Flags [7:4]
4       1     Sequence number
5       1     Reserved (0)
6       2     Payload length, big-endian (0–4096)
8       N     Payload
8+N     2     CRC16-CCITT of bytes [0..7+N], big-endian
```

### Commands

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| 0x00 | NOOP | both | Padding frame — no data |
| 0x01 | DATA | both | Serial payload |
| 0x02 | CONNECT | MCU→Host | MCU identifies itself; payload = 6-byte MAC |
| 0x03 | DISCONNECT | MCU→Host | MCU disconnecting |
| 0x04 | STATUS_REQ | Pi→Host | Request connected MCU bitmask |
| 0x05 | STATUS_RSP | Host→Pi | Connected MCU bitmask response |
| 0x06 | RESET | both | Request peer reset / flush |
| 0x07 | ACK | both | Acknowledge (seq echo) |

### MCU ID derivation

```python
# Equivalent Python
import struct, hashlib

def mac_to_mcu_id(mac_bytes: bytes, max_mcu: int = 8) -> int:
    """FNV-1a 32-bit hash of MAC[3:5], folded to [0, max_mcu)."""
    h = 2166136261
    for b in mac_bytes[3:6]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h % max_mcu
```

### CRC

CRC16-CCITT: polynomial `0x1021`, initial value `0xFFFF`, no final XOR.
Covers all bytes before the 2-byte CRC field.

---

## 13. Troubleshooting

### Host ESP won't boot / no serial output

- Check that you flashed `host_esp`, not `mcu_esp`.
- Use the bottom USB connector on the compact board (USB Serial/JTAG).
- Hold BOOT, press RESET, release BOOT to enter bootloader mode manually.

### MCU ESP won't connect to WiFi

- Confirm the host ESP is powered and running (its LED or the serial monitor).
- The SSID is **hidden** — the MCU must know it in advance (it does, it's
  compiled in).
- WPA3-SAE requires both sides to support it; the ESP32-C5 does.
- Check channel 6 is not blocked by local interference; change
  `KWM_WIFI_CHANNEL` in `kwm_protocol.h` and rebuild both firmwares.

### MCU IDs colliding (two boards, same ID)

See end of [section 6](#6-build-and-flash--mcu-esp32-c5).  Check both boards'
boot logs to confirm which MACs are colliding.

### Klipper reports "Unable to connect" on `/dev/kwmN`

1. Confirm the bridge daemon is running: `sudo systemctl status kwm-bridge`.
2. Confirm the symlink exists: `ls -la /dev/kwmN`.
3. Confirm the MCU ESP has connected — check host serial monitor for the
   `MCU N connected` log line.
4. Check that the ID in `printer.cfg` matches the ID the board actually got
   (read its boot log).

### Klipper serial errors / "Protocol error"

- This almost always means a byte was lost or corrupted in transit.
- Check SPI wiring — use short wires (<20 cm), ensure good ground between Pi
  and Host ESP.
- The SPI clock is 10 MHz; if you see intermittent errors, try 4 MHz by
  passing `--spi-speed 4000000` to the bridge daemon.
- Check that `TCP_NODELAY` is in effect (it is by default in this firmware).
  Without it, Nagle's algorithm introduces ~40 ms buffering per message.

### High latency / Klipper timing errors

- Ensure the MCU ESP is running with `WIFI_PS_NONE` (it is by default in this
  firmware).  If you compiled your own build and see latency, check that
  `esp_wifi_set_ps(WIFI_PS_NONE)` is called in `wifi_sta.c`.
- Move the host ESP closer to the MCU ESPs, or reduce interference on channel 6.
- WiFi 6 OFDMA means multiple MCUs transmit simultaneously on sub-carriers;
  latency should be consistent even with several boards.

### TCP connection keeps dropping

- The firmware uses keepalive: idle=5 s, interval=2 s, count=3.  A dead
  connection is detected in ~11 seconds, after which the MCU ESP automatically
  reconnects.
- If connections drop more often, check WiFi signal strength on the MCU ESP
  serial monitor — RSSI should be above −70 dBm for reliable operation.

---

## 14. Adding More MCUs

1. Flash a new ESP32-C5 DevKitC-1 with the `mcu_esp` binary (same file, no
   re-build needed).
2. Wire it to the new STM32 UART (TX→GPIO16, RX←GPIO17, common GND).
3. Power it via USB.
4. Read its boot log to find its auto-assigned MCU ID:
   ```
   I main: MCU ID = 5  (hash of MAC[3:5] mod 8)
   I main: Pi PTY will appear as /dev/kwm5
   ```
5. Add the new ID to the bridge daemon's `--mcus` list and restart it:
   ```bash
   sudo systemctl edit kwm-bridge
   # Change: ExecStart=... --mcus 0 1 5
   sudo systemctl restart kwm-bridge
   ```
6. Add the new MCU to `printer.cfg`:
   ```ini
   [mcu mcu5]
   serial: /dev/kwm5
   ```
7. Restart Klipper.

No reflashing of any existing board is required.
