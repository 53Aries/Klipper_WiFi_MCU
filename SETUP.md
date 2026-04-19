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
│  klipper_bridge.py  ←──── uart_driver.py                        │
│       │                        │                                 │
│  GPIO4/TXD (pin 7)        /dev/ttyAMA2  (1 Mbaud)               │
│  GPIO5/RXD (pin 29)                                              │
└──────────────────────────────┬──────────────────────────────────┘
                               │  256-byte KWM frames @ 1 Mbaud
                               │  UART, 8N1, no flow control
                    ┌──────────┴──────────┐
                    │  Host XIAO ESP32-C5  │  Seeed XIAO ESP32-C5
                    │  WiFi 6 soft-AP      │  8 MB flash
                    │  TCP server :8842    │  192.168.42.1
                    └──────────┬──────────┘
                               │  WiFi 6  (802.11ax, OFDMA)
                               │  hidden SSID "KlipperMesh"
              ┌────────────────┼────────────────┐
              │                │                │
   ┌──────────┴──────┐ ┌───────┴───────┐  (up to 8 MCU ESPs)
   │ MCU XIAO #0     │ │ MCU XIAO #1   │
   │ WiFi 6 STA      │ │ WiFi 6 STA    │
   │ TCP client      │ │ TCP client    │
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
2. `klipper_bridge.py` reads from the PTY master, packs a 256-byte KWM frame.
3. Pi5 sends the frame to the Host XIAO over UART (`/dev/ttyAMA2`, 1 Mbaud).
4. Host ESP routes the payload over TCP to the matching MCU ESP connection.
5. MCU ESP writes the bytes out UART1 to the STM32.

### Data path (MCU → Pi)

1. STM32 sends bytes over UART to MCU ESP.
2. MCU ESP wraps them in a TCP frame and sends to Host ESP.
3. Host ESP builds a 256-byte KWM frame and writes it to its UART TX.
4. Pi5 reads the frame from `/dev/ttyAMA2`, unpacks it.
5. `klipper_bridge.py` writes the payload to the correct PTY master.
6. Klipper reads the bytes from the PTY slave `/dev/kwm0`.

---

## 2. Hardware Required

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | **Raspberry Pi 5** (2 GB+ RAM) | Host computer running Klipper |
| 1 | **Seeed XIAO ESP32-C5** | Host ESP — UART to Pi, WiFi AP |
| 1+ | **Seeed XIAO ESP32-C5** | MCU ESP — one per Klipper MCU |
| 1+ | **STM32 Klipper MCU board** | e.g. SKR, Octopus, etc. |
| — | 3 dupont wires | TX/RX/GND from Pi to Host XIAO |
| — | 2 dupont wires (+ GND) | TX/RX from MCU XIAO to STM32 |

> **Both the host-side and MCU-side boards are the same hardware** — a Seeed
> XIAO ESP32-C5.  Flash `host_esp` firmware to the Pi-facing board and
> `mcu_esp` firmware to each STM32-facing board.

> **Up to 8 MCU-side XIAOs** are supported simultaneously via WiFi 6 OFDMA.

---

## 3. Wiring

### Pi5 ↔ Host XIAO ESP32-C5 (UART)

3 wires total.  Cross TX↔RX as normal for UART.

```
Pi 5 Header       GPIO    XIAO ESP32-C5 Pin   Signal
─────────────────────────────────────────────────────
Pin  7  (TXD)     GPIO4 → D7 / GPIO12 (RX)    Pi→ESP data
Pin 29  (RXD)     GPIO5 ← D6 / GPIO11 (TX)    ESP→Pi data
Pin  6  (GND)     GND   — GND                 Common ground
```

> The Host XIAO is powered via its USB-C connector.  No power wire from the
> Pi is needed (and none should be connected if the XIAO is USB-powered).

### MCU XIAO ESP32-C5 ↔ STM32 (UART)

```
XIAO ESP32-C5 Pin   STM32 Pin      Signal
──────────────────────────────────────────
D6 / GPIO11 (TX) →  UART RX pin    Serial data ESP→STM32
D7 / GPIO12 (RX) ←  UART TX pin    Serial data STM32→ESP
GND              —   GND           Common ground
```

> Baud rate: **250 000** (standard Klipper serial rate; compiled into firmware).

---

## 4. Software Prerequisites

### On the build machine (Windows/Linux/macOS)

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html):

```powershell
# Windows PowerShell
pip install platformio
```

Install the pioarduino platform (IDF 5.5 + RISC-V toolchain — ~700 MB, one-time):

```powershell
pio pkg install -g -p "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip"
```

### On the Pi5

See [section 7](#7-pi5-setup) for the full Pi5 setup walkthrough.

---

## 5. Build and Flash — Host ESP32-C5

The host firmware runs on the XIAO ESP32-C5 board connected to the Pi via UART.

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

Binary at: `.pio/build/host_esp/firmware.factory.bin`

### Flash

Connect the Host XIAO to your PC via USB-C.

```powershell
pio run -e host_esp -t upload
```

On Linux, if you get a permission error on the serial device:

```bash
sudo usermod -aG dialout $USER   # log out and back in
pio run -e host_esp -t upload
```

### Verify

```powershell
pio device monitor -e host_esp
```

Expected output on boot:

```
I (xxx) main: === Klipper WiFi MCU - Host ESP32-C5 ===
I (xxx) main: IDF 5.5.4 | cores=1
I (xxx) main: TCP port 8842 | max MCUs 8
I (xxx) kwm_uart: UART1 ready (TX=11 RX=12 baud=1000000)
I (xxx) wifi_ap: WiFi 6 AP started: SSID=KlipperMesh (hidden) channel=6 IP=192.168.42.1
I (xxx) tcp_server: TCP server listening on port 8842
I (xxx) bridge: Bridge initialised
I (xxx) main: Host firmware ready. Waiting for Pi UART and MCU connections.
```

---

## 6. Build and Flash — MCU ESP32-C5 (XIAO)

**All MCU-side boards use the same firmware binary.**  Each derives its own
MCU ID (0–7) at runtime from its hardware MAC address.

### Build

```powershell
pio run -e mcu_esp
```

Binary at: `.pio/build/mcu_esp/firmware.factory.bin`

### Flash (repeat for every MCU XIAO)

```powershell
pio run -e mcu_esp -t upload
```

### Verify

```powershell
pio device monitor -e mcu_esp
```

Expected output:

```
I (xxx) main: === Klipper WiFi MCU - MCU Bridge ESP32-C5 ===
I (xxx) main: IDF v5.5.4
I (xxx) main: MAC  ac:15:18:3a:b2:7f
I (xxx) main: MCU ID = 3  (hash of MAC[3:5] mod 8)
I (xxx) main: Pi PTY will appear as /dev/kwm3
I (xxx) wifi_sta: Connected to AP 'KlipperMesh'
I (xxx) tcp_client: TCP connected to host (fd=4)
I (xxx) tcp_client: Sent CONNECT: mcu_id=3  MAC=ac:15:18:3a:b2:7f
```

Record the MAC → ID mapping for each board so you know which PTY to reference
in `printer.cfg`.

### If two boards collide on the same ID

Collision probability is low (≈12 % with 2 boards).  If it occurs, both boards
log the same ID and one will kick the other off the host.  Override one board's
ID via NVS at the monitor prompt:

```
nvs_set kwm mcu_id u8 5
```

*(NVS override support is planned for a future firmware revision.)*

---

## 7. Pi5 Setup

Work through these steps on a **fresh Pi OS Lite (64-bit) install**.

### Step 1 — Update the system

```bash
sudo apt update && sudo apt upgrade -y
sudo reboot
```

### Step 2 — Install dependencies

```bash
sudo apt install -y python3-serial git
```

Verify Python 3 is available:

```bash
python3 --version   # Python 3.11.x or newer
```

### Step 3 — Disable Bluetooth to free the hardware UART

The Pi5's Bluetooth controller holds the primary UART by default.
Disable it, enable UART2 on GPIO4/5, and reboot:

```bash
sudo tee -a /boot/firmware/config.txt << 'EOF'
dtoverlay=disable-bt-pi5
dtoverlay=uart2-pi5
EOF
sudo reboot
```

After reboot, verify the UART device is available:

```bash
ls /dev/ttyAMA2   # should exist
```

And confirm Bluetooth is no longer holding the UART:

```bash
sudo systemctl status hciuart   # should show "masked" or "not-found"
```

### Step 4 — Add your user to the `dialout` group

`/dev/serial0` is owned by `root:dialout`.  The bridge service runs as root so
this is only needed for manual testing without `sudo`:

```bash
sudo usermod -aG dialout $USER
# Log out and back in, then verify:
groups   # should include dialout
```

### Step 5 — Clone the repository

```bash
cd ~
git clone --branch UART --single-branch https://github.com/53Aries/Klipper_WiFi_MCU.git
cd Klipper_WiFi_MCU
```

### Step 6 — Install the bridge service

Run these two commands from inside the `Klipper_WiFi_MCU` directory:

```bash
REPO="$HOME/Klipper_WiFi_MCU"
```

```bash
sudo tee /etc/systemd/system/kwm-bridge.service > /dev/null << UNIT
[Unit]
Description=Klipper WiFi MCU UART bridge daemon
After=network.target
Before=klipper.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 ${REPO}/pi_host/klipper_bridge.py
Restart=always
RestartSec=3
User=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNIT
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable kwm-bridge
sudo systemctl start kwm-bridge
sudo systemctl status kwm-bridge
```

Check logs:

```bash
sudo journalctl -u kwm-bridge -f
```

---

## 8. Running the Bridge Daemon

> The systemd service (section 7, step 6) is the normal way to run the bridge.
> Use the manual command below only for debugging.

```bash
sudo python3 pi_host/klipper_bridge.py
```

No arguments needed for default operation.  The daemon pre-creates PTY
symlinks for all 8 MCU IDs (`/dev/kwm0`–`/dev/kwm7`) at startup.

### All options

```
--mcus ID [ID ...]     MCU IDs to create PTYs for (default: 0-7)
--port PATH            UART device (default: /dev/ttyAMA2)
--baudrate N           Baud rate (default: 1000000)
--verbose / -v         Enable debug logging
```

### Expected startup output

```
2026-04-18 12:00:01 klipper_bridge INFO UART opened: /dev/ttyAMA2 @ 1000000 baud
2026-04-18 12:00:01 klipper_bridge INFO MCU 0 PTY: /dev/pts/2
2026-04-18 12:00:01 klipper_bridge INFO MCU 0 symlink: /dev/kwm0 -> /dev/pts/2
...
2026-04-18 12:00:01 klipper_bridge INFO Bridge running. MCUs: [0, 1, 2, 3, 4, 5, 6, 7]
```

When an MCU ESP connects over WiFi:

```
2026-04-18 12:00:05 klipper_bridge INFO MCU 3 connected (MAC: ac:15:18:3a:b2:7f)
```

---

## 9. Klipper Configuration

Edit `~/printer_data/config/printer.cfg`.

### Single additional MCU

```ini
[mcu secondary]
serial: /dev/kwm0
```

### Multiple MCUs

```ini
[mcu mcu0]
serial: /dev/kwm0

[mcu mcu3]
serial: /dev/kwm3
```

Reference MCU-specific pins as usual:

```ini
[stepper_x]
step_pin: mcu0:PA1
dir_pin:  mcu0:PA2
```

### Notes

- The bridge daemon must be running before Klipper attempts to connect.  The
  systemd service ordering (`Before=klipper.service`) handles this automatically.
- Klipper reconnects automatically if the link drops — the PTY stays open on
  the Pi side; only the WiFi/TCP path reconnects behind the scenes.

---

## 10. Verifying Operation

### Check bridge logs

```bash
sudo journalctl -u kwm-bridge -f
```

### Check host ESP serial monitor

```powershell
pio device monitor -e host_esp
```

Expected periodic status line:

```
I main: Heap free: 284312 bytes | UART rx pending: 0 | WiFi STAs: 2
```

### Check MCU ESP serial monitor

```powershell
pio device monitor -e mcu_esp
```

### Confirm PTY symlinks on Pi

```bash
ls -la /dev/kwm*
# lrwxrwxrwx 1 root root 10 ... /dev/kwm0 -> /dev/pts/2
```

### Send a test frame from the Pi

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, 'pi_host')
from uart_driver import UartDriver
d = UartDriver()
d.open()
d.send(mcu_id=0, data=b'\x01\x02\x03')
print("sent")
d.close()
EOF
```

---

## 11. Systemd Service (Auto-start)

See [section 7, step 6](#step-6--install-the-bridge-service).

---

## 12. Protocol Reference

### WiFi network

| Parameter | Value |
|-----------|-------|
| SSID | `KlipperMesh` (hidden) |
| Password | `klipper42!` |
| Security | WPA3-SAE |
| Band | 2.4 GHz, channel 6 |
| Standard | 802.11ax (WiFi 6) with OFDMA |
| AP IP | `192.168.42.1` |
| TCP port | `8842` |

### UART frame (Pi ↔ Host ESP)

Fixed 256 bytes per frame.  The Pi sends one frame per chunk of Klipper serial
data; the ESP sends one frame per TCP payload received from a MCU.

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

- Confirm you flashed `host_esp`, not `mcu_esp`.
- Use the USB-C connector on the XIAO (USB Serial/JTAG port).
- Hold BOOT, press RESET, release BOOT to enter bootloader manually if needed.

### No UART communication (Pi ↔ Host ESP)

- Confirm both `dtoverlay=disable-bt-pi5` and `dtoverlay=uart2-pi5` are in
  `/boot/firmware/config.txt` and the Pi has been rebooted.
- The UART device is `/dev/ttyAMA2` (GPIO4/5, Pi header pins 7 and 29).
- Verify TX and RX are **crossed**: Pi GPIO4/TXD (pin 7) → XIAO D7/RX (GPIO12) and
  Pi GPIO5/RXD (pin 29) ← XIAO D6/TX (GPIO11).
- Confirm `/dev/ttyAMA2` exists: `ls /dev/ttyAMA2`.
- Run the bridge with `--verbose` and check for "UART opened" in the logs.

### MCU ESP won't connect to WiFi

- Confirm the host ESP is powered and running (check its serial monitor).
- The SSID is **hidden** — the MCU firmware knows it at compile time.
- Check channel 6 is not blocked; change `KWM_WIFI_CHANNEL` in
  `kwm_protocol.h` and rebuild both firmwares.

### Klipper reports "Unable to connect" on `/dev/kwmN`

1. `sudo systemctl status kwm-bridge` — confirm the daemon is running.
2. `ls -la /dev/kwmN` — confirm the symlink exists.
3. Check bridge logs for the `MCU N connected` event.
4. Confirm the ID in `printer.cfg` matches the ID the board actually got
   (read its boot log).

### Klipper serial errors / "Protocol error"

- Usually a dropped byte.  Check UART wiring — keep wires under 20 cm.
- Confirm baud rate matches: both sides must be 1 000 000.
- If intermittent, try running the bridge with `--baudrate 500000` and rebuild
  firmware with `KWM_HOST_UART_BAUD 500000`.

### High latency / Klipper timing errors

- Ensure MCU ESP runs with `WIFI_PS_NONE` (default in this firmware).
- Move the host ESP closer to the MCU ESPs, or reduce interference on channel 6.
- Check MCU ESP RSSI: should be above −70 dBm for reliable operation.

### TCP connection keeps dropping

- Firmware uses keepalive: idle=5 s, interval=2 s, count=3.  Dead connections
  are detected in ~11 s and the MCU ESP reconnects automatically.

---

## 14. Adding More MCUs

1. Flash a new XIAO ESP32-C5 with `mcu_esp` firmware (same binary, no rebuild).
2. Wire it to the STM32 UART: TX→D6/GPIO11, RX←D7/GPIO12, common GND.
3. Power via USB.
4. Read its boot log to find its auto-assigned MCU ID:
   ```
   I main: MCU ID = 5  (hash of MAC[3:5] mod 8)
   ```
5. The bridge daemon already has `/dev/kwm5` ready — no restart needed.
6. Add to `printer.cfg`:
   ```ini
   [mcu mcu5]
   serial: /dev/kwm5
   ```
7. Restart Klipper.

No reflashing of existing boards and no daemon reconfiguration required.
