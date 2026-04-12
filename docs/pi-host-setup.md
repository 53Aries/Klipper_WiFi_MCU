# Pi Host Setup Guide

This document covers everything needed to configure a Raspberry Pi 5 as the Klipper WiFi host — from wiring through to a working Klipper connection.

**System context:** The XIAO ESP32-C5 plugged into the Pi runs ESP-Hosted firmware, which makes the Pi's `wlan0` interface a real WiFi adapter. The Pi then hosts a hidden 5GHz AP (`klipper-mcu-net`) that the MCU-side XIAO boards connect to. Klipper talks to each MCU via `socket://192.168.42.x:23`, and has no idea WiFi is involved.

---

## Prerequisites

- Raspberry Pi 5 running **Pi OS Bookworm** (64-bit recommended)
- XIAO ESP32-C5 soldered and wired per [docs/spi-pinout.md](spi-pinout.md)
- USB cable to flash the XIAO from a PC (only needed once)
- Internet access on the Pi (via its onboard NIC, not wlan0)

---

## Step 1 — Flash the ESP-Hosted firmware

Do this **on your PC** before connecting the XIAO to the Pi.

1. Open the `esp-hosted-pi` project in VS Code / PlatformIO
2. Enter flash mode on the XIAO: **hold BOOT, tap RESET, release BOOT**
3. Run **Upload** in PlatformIO (port auto-detects, or set `upload_port` in `platformio.ini`)
4. After flashing succeeds, disconnect the USB cable

The XIAO is now running ESP-Hosted-FG. It will sit idle until the Pi drives it over SPI.

---

## Step 2 — Wire the XIAO to the Pi

Connect the XIAO to the Pi 40-pin header as documented in [docs/spi-pinout.md](spi-pinout.md).

Quick summary:

| Pi physical pin | Signal    | XIAO pin |
|-----------------|-----------|----------|
| 19 (SPI0 MOSI)  | MOSI      | IO7      |
| 21 (SPI0 MISO)  | MISO      | IO2      |
| 23 (SPI0 SCLK)  | SCLK      | IO6      |
| 24 (SPI0 CE0)   | CS        | IO10     |
| 13 (BCM27/#539) | Data Ready| IO4      |
| 15 (BCM22/#534) | Handshake | IO3      |
| 31 (BCM6/#518)  | RESET     | RST      |
| 25 (GND)        | GND       | GND      |
| 1 or 17 (3V3)   | Power     | 3V3      |

> **Do not power the XIAO from 5V.** Use the Pi's 3.3 V rail (pin 1 or 17).

---

## Step 3 — Set up ESP-Hosted on the Pi

SSH into the Pi and run the setup script:

```bash
cd ~/KlipperESPwifi/pi-host/scripts   # or wherever you've cloned the repo
chmod +x setup-esp-hosted.sh
./setup-esp-hosted.sh
```

**What the script does:**
- Installs `raspberrypi-kernel-headers`, `build-essential`, `git`
- Adds `dtparam=spi=on` and `dtoverlay=disable-bt` to `/boot/firmware/config.txt`
- Clones the `esp-hosted` repository from Espressif
- Builds the `esp32_spi` kernel module
- Loads it with the correct GPIO parameters for our wiring:
  - `resetpin=518` — BCM GPIO6, physical pin 31 → XIAO RST
  - `spi-handshake=534` — BCM GPIO22, physical pin 15 → XIAO IO3
  - `spi-dataready=539` — BCM GPIO27, physical pin 13 → XIAO IO4

> **Reboot required** after this script — the SPI `dtparam` only takes effect on next boot.

```bash
sudo reboot
```

---

## Step 4 — Verify ESP-Hosted is running

After reboot, SSH back in and check:

```bash
lsmod | grep esp
# Expected: esp32_spi   <size>   0

ip link show wlan0
# Expected: wlan0: <BROADCAST,MULTICAST> ...

dmesg | grep -i esp
# Should show "ESP-Hosted: SPI transport up" or similar
```

If `wlan0` does not appear:
- Re-check wiring against [docs/spi-pinout.md](spi-pinout.md)
- Confirm SPI is enabled: `ls /dev/spi*` should show `/dev/spidev0.0`
- Check `dmesg | tail -30` for probe errors

---

## Step 5 — Edit hostapd.conf before starting the AP

**Set your WiFi password** in [pi-host/hostapd/hostapd.conf](../pi-host/hostapd/hostapd.conf):

```
wpa_passphrase=CHANGE_ME   ← replace this
```

Match the same password in all `esp-bridge` firmware configs — see [Step 8](#step-8--configure-and-flash-esp-bridge).

Optionally change `channel=36` to `149` if 36 is congested in your environment. The MCU board firmware reads the channel from `config.h` (`WIFI_CHANNEL`), so keep them consistent.

---

## Step 6 — Start the AP

```bash
cd ~/KlipperESPwifi/pi-host/scripts
chmod +x setup-ap.sh
./setup-ap.sh
```

**What the script does:**
- Installs `hostapd` and `dnsmasq`
- Tells NetworkManager to leave `wlan0` alone
- Assigns static IP `192.168.42.1/24` to `wlan0`
- Copies `hostapd.conf` → `/etc/hostapd/hostapd.conf`
- Copies `dnsmasq.conf` → `/etc/dnsmasq.conf`
- Copies `sysctl-klipper.conf` → `/etc/sysctl.d/99-klipper.conf` and applies it
- Creates a persistent NetworkManager profile to hold the static IP across reboots
- Enables and starts `hostapd` + `dnsmasq`

Verify it worked:

```bash
sudo systemctl status hostapd
sudo systemctl status dnsmasq
ip addr show wlan0   # should show 192.168.42.1/24
```

---

## Step 7 — Get the Pi's wlan0 MAC address

You need this to pin the MCU boards to the Pi's AP (faster reconnect):

```bash
ip link show wlan0 | grep ether
# Example: link/ether dc:a6:32:xx:xx:xx brd ff:ff:ff:ff:ff:ff
```

Note this MAC — you'll use it in `config.h` in the next step.

You also need the MAC of each MCU-side XIAO board to create static DHCP leases. Get them after the first successful connection (see Step 9).

---

## Step 8 — Configure and flash esp-bridge

Edit [esp-bridge/main/config.h](../esp-bridge/main/config.h) on your PC:

```c
#define WIFI_PASSWORD    "your-real-password"   // must match hostapd.conf

// Set the Pi's wlan0 MAC from Step 7:
#define AP_BSSID         {0xdc, 0xa6, 0x32, 0xx, 0xx, 0xx}
#define AP_BSSID_SET     true

// Confirm channel matches hostapd.conf:
#define WIFI_CHANNEL     36

// Confirm the static IP matches the dnsmasq lease you'll add in Step 9:
#define STATIC_IP        "192.168.42.11"   // (or .12, .13 for other boards)
```

Then flash each XIAO that's connected to an MCU board:

1. Hold BOOT on the XIAO, tap RESET, release BOOT
2. Run **Upload** in PlatformIO
3. After flashing, the XIAO will reset and attempt to connect to the AP

---

## Step 9 — Add static DHCP leases

Once a board connects, find its MAC via dnsmasq's lease file:

```bash
cat /var/lib/misc/dnsmasq.leases
# Format: <expiry> <mac> <ip> <hostname> <client-id>
```

Add a static lease for each board in [pi-host/dnsmasq/dnsmasq.conf](../pi-host/dnsmasq/dnsmasq.conf):

```
dhcp-host=aa:bb:cc:dd:ee:11,mcu-toolhead,192.168.42.11
dhcp-host=aa:bb:cc:dd:ee:12,mcu-aux,192.168.42.12
```

Then deploy and restart:

```bash
sudo cp ~/KlipperESPwifi/pi-host/dnsmasq/dnsmasq.conf /etc/dnsmasq.conf
sudo systemctl restart dnsmasq
```

Verify connectivity from the Pi:

```bash
ping -c 3 192.168.42.11
```

---

## Step 10 — Klipper configuration

Add to your `printer.cfg`:

```ini
[include mcu-wifi.cfg]
```

Or reference the socket directly. The [klipper-config/mcu-wifi.cfg](../klipper-config/mcu-wifi.cfg) snippet:

```ini
[mcu toolhead]
serial: socket://192.168.42.11:23
restart_method: command

[mcu aux]
serial: socket://192.168.42.12:23
restart_method: command
```

Restart Klipper and check its log:

```bash
sudo journalctl -u klipper -f
# Look for "mcu toolhead: connected" without errors
```

---

## Troubleshooting

### wlan0 not appearing after reboot
- `lsmod | grep esp` — if empty, module did not load. Re-run `./setup-esp-hosted.sh`
- `ls /dev/spidev*` — if missing, SPI overlay not applied. Check `/boot/firmware/config.txt` for `dtparam=spi=on`, then reboot
- `dmesg | grep -E 'esp|spi'` — look for probe errors

### hostapd fails to start
- `sudo journalctl -u hostapd -n 50` — common cause: another process owns `wlan0`
- `sudo nmcli dev status` — if wlan0 shows "connected", run `sudo nmcli dev set wlan0 managed no`
- Check that `wpa_passphrase` is set (not `CHANGE_ME`) in `/etc/hostapd/hostapd.conf`

### MCU board not connecting
- Check WIFI_PASSWORD in `config.h` matches `hostapd.conf`
- Check WIFI_CHANNEL matches
- Confirm `AP_BSSID_SET = false` for initial testing — enable pinning only after confirming it connects
- ESP-IDF logs: connect XIAO to PC USB while powered from MCU, open serial monitor at 115200

### Klipper "Unable to connect" on socket://
- `ping 192.168.42.11` from Pi — if no reply, board not connected to AP yet
- Check bridge is listening: from Pi, `telnet 192.168.42.11 23` (type a byte — STM32 echo if MCU connected)
- Confirm port 23 is not firewalled: `sudo iptables -L INPUT -n | grep 23`

### Static IP not persisting after reboot
- `ip addr show wlan0` — if address is missing, the NetworkManager profile may not have applied
- Re-run `./setup-ap.sh` or manually: `sudo nmcli con up wlan0-static`

---

## Maintenance notes

- **Platform patches will be lost on `pio pkg update`** for the `espressif32` platform. If PlatformIO updates the platform and builds break, re-apply the patches documented in the relevant commit / save them to a separate script.
- **hostapd.conf and dnsmasq.conf** in `pi-host/` are the source of truth. After editing them on your PC, re-run the copy commands in Step 9 or re-run `setup-ap.sh`.
- **BSSID pinning** (`AP_BSSID_SET = true`) significantly speeds up reconnect after an MCU reset. Set it for production once the Pi MAC is stable.
