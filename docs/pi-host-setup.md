# Pi Host Setup Guide

This guide takes you from a **brand-new Raspberry Pi OS install** to a fully working Klipper WiFi MCU setup. Follow every step in order.

**What you need physically:**
- Raspberry Pi 5 running **Pi OS Bookworm or Trixie** (64-bit), already booted and on your local network
- ESP32-C5 DevKit V2.0 (pi-side) already flashed with the `esp-hosted-pi` firmware (do that on your PC first — see below)
- Jumper wires to connect the devkit to the Pi 40-pin header
- A Seeed XIAO ESP32-C5 (MCU-side) connected to your STM32 MCU board, already flashed with `esp-bridge` firmware
- SSH access to the Pi from your PC

---

## Before you start — flash the ESP boards on your PC

Do both of these on your Windows PC **before** touching the Pi.

### Pi-side DevKit (esp-hosted-pi firmware)

1. Open the `esp-hosted-pi` folder in VS Code / PlatformIO
2. Hold **BOOT** on the devkit, tap **RESET**, release **BOOT** — the board is now in flash mode
3. Run **Upload** in PlatformIO
4. When it finishes, disconnect the USB cable — the devkit is ready

### MCU-side XIAO (esp-bridge firmware)

1. Open the `esp-bridge` folder in VS Code / PlatformIO
2. Verify [esp-bridge/main/config.h](../esp-bridge/main/config.h) looks like this (already set):
   ```c
   #define WIFI_SSID     "klipper"
   #define WIFI_PASSWORD "klipper"
   #define STATIC_IP     "192.168.42.11"
   ```
3. Hold **BOOT**, tap **RESET**, release **BOOT**
4. Run **Upload** in PlatformIO
5. Disconnect USB — the XIAO is ready

> For a second MCU board, change `STATIC_IP` to `"192.168.42.12"`, re-flash, and repeat.

---

## Step 1 — Wire the pi-side DevKit to the Pi 40-pin header

The ESP32-C5 DevKit connects to the Pi via SPI. All signals use edge connector pins on the devkit.

Almost all wires land on the **left column** of the Pi header (odd pins). Pin 4 (5V) and Pin 18 (Handshake) and Pin 24 (CS) are on the right.

| Pi physical pin | Side  | Signal     | DevKit pin     |
|-----------------|-------|------------|----------------|
| Pin 4           | Right | 5V         | 5V             |
| Pin 9           | Left  | GND        | GND            |
| Pin 11          | Left  | Reset      | RST            |
| Pin 13          | Left  | Data Ready | GPIO4          |
| Pin 18          | Right | Handshake  | GPIO3          |
| Pin 19          | Left  | SPI0 MOSI  | GPIO7          |
| Pin 21          | Left  | SPI0 MISO  | GPIO2          |
| Pin 23          | Left  | SPI0 SCLK  | GPIO6          |
| Pin 24          | **Right** | SPI0 CS | GPIO10      |

All nine connections use the devkit’s edge connector pins — no bottom-pad soldering required.

> The devkit’s 3V3 pin is regulator **output** only — do not connect it to the Pi. Use the 5V pin (onboard regulator steps it down).

Full details with signal directions and driver GPIO numbers: [docs/spi-pinout.md](spi-pinout.md)

---

## Step 2 — Get the project onto the Pi

SSH into the Pi from your PC (replace `raspberrypi.local` with your Pi's hostname or IP):

```bash
ssh pi@raspberrypi.local
```

Once logged in, clone this repo:

```bash
cd ~
git clone https://github.com/53Aries/Klipper_WiFi_MCU.git KlipperESPwifi
```

> If you haven't pushed the repo to GitHub yet, copy it over with `scp` or a USB drive instead:
> ```bash
> # Run this on your PC (PowerShell), not the Pi
> scp -r C:\KlipperESPwifi pi@raspberrypi.local:~/KlipperESPwifi
> ```

---

## Step 3 — Run the ESP-Hosted setup script

This installs the SPI kernel driver that makes the XIAO appear as `wlan0`.

```bash
cd ~/KlipperESPwifi/pi-host/scripts
chmod +x setup-esp-hosted.sh
./setup-esp-hosted.sh
```

**What it does:**
- Installs `git`, `build-essential`, `linux-headers-$(uname -r)`
- Compiles and installs a `spidev-disabler` boot overlay that targets `&spidev10` — disabling only the spidev child device on SPI bus 10, leaving the bus controller itself free for the `esp32_spi` module
- Adds `dtoverlay=disable-bt` to `/boot/firmware/config.txt`
- Clones Espressif's `esp-hosted` repo and builds the `esp32_spi` kernel module
- Loads the module with the correct GPIO numbers for our wiring

The script will print a wiring summary at the end. **SPI is not active until you reboot:**

```bash
sudo reboot
```

---

## Step 4 — Verify the XIAO is visible as wlan0

After the reboot, SSH back in:

```bash
ssh pi@raspberrypi.local
```

Run these checks:

```bash
lsmod | grep esp
```
Expected output: a line containing `esp32_spi`

```bash
ip link show wlan0
```
Expected output: a line beginning `2: wlan0: <BROADCAST,MULTICAST>...`

```bash
dmesg | grep -i esp | tail -10
```
Expected: messages about SPI transport initialising

**If `wlan0` does not appear:**
- Check wiring: re-read [docs/spi-pinout.md](spi-pinout.md) pin-by-pin
- Module loaded? `lsmod | grep esp` — if missing, run `sudo modprobe esp32_spi` and check `dmesg | tail -20`
- SPI bus alive? `ls /sys/bus/spi/devices/` should show `spi10.0`
- DataReady firing? `cat /proc/interrupts | grep ESP` — ESP_SPI_DATA_READY must have count > 0
- Check for errors: `dmesg | grep -iE 'esp|spi|fail' | tail -30`

---

## Step 5 — Start the WiFi AP

This sets up `wlan0` as a WiFi 6 access point. The MCU boards will connect to it.

```bash
cd ~/KlipperESPwifi/pi-host/scripts
chmod +x setup-ap.sh
./setup-ap.sh
```

**What it does:**
- Installs `hostapd` and `dnsmasq`
- Tells NetworkManager to leave `wlan0` alone so `hostapd` can own it
- Assigns static IP `192.168.42.1/24` to `wlan0`
- Copies `hostapd.conf` and `dnsmasq.conf` from this repo to `/etc/`
- Applies TCP tuning to reduce latency (disables Nagle algorithm)
- Creates a persistent NM profile so the static IP survives reboots
- Enables and starts `hostapd` and `dnsmasq`

**AP credentials (already configured):**
- SSID: `klipper`
- Password: `klipper`
- Channel: 36 (5 GHz)
- SSID is hidden (won't appear in normal WiFi scans)

**Verify it worked:**

```bash
sudo systemctl status hostapd
# Should show: Active: active (running)

sudo systemctl status dnsmasq
# Should show: Active: active (running)

ip addr show wlan0
# Should show: inet 192.168.42.1/24
```

**If hostapd fails to start:**
```bash
sudo journalctl -u hostapd -n 30
```
- If the log says `wlan0: driver does not support configuration of vlan` or similar, re-check the XIAO wiring — hostapd cannot start without a working wlan0
- If another process owns wlan0: `sudo nmcli dev set wlan0 managed no` then re-run `./setup-ap.sh`

---

## Step 6 — Power up and connect an MCU board

1. Connect the MCU-side XIAO to your STM32 board (or just power it standalone to test)
2. Power the MCU board on

The XIAO will boot and attempt to join the `klipper` network automatically. This usually takes 3–10 seconds.

Watch for it from the Pi:

```bash
watch -n 1 cat /var/lib/misc/dnsmasq.leases
```

When the board connects you'll see a line like:

```
1744600000 aa:bb:cc:dd:ee:ff 192.168.42.10 esp-bridge *
```

Note the MAC address (`aa:bb:cc:dd:ee:ff` in this example) — you need it in the next step.

Press `Ctrl+C` to stop watching.

---

## Step 7 — Add a static DHCP lease for the MCU board

Open the dnsmasq config on the Pi:

```bash
nano ~/KlipperESPwifi/pi-host/dnsmasq/dnsmasq.conf
```

Find the commented-out static lease lines and add your board's MAC:

```
dhcp-host=aa:bb:cc:dd:ee:ff,mcu-toolhead,192.168.42.11
```

Replace `aa:bb:cc:dd:ee:ff` with the actual MAC from Step 6.  
The IP `192.168.42.11` must match `STATIC_IP` in the board's `config.h`.

Save the file (`Ctrl+O`, `Ctrl+X`), then deploy it:

```bash
sudo cp ~/KlipperESPwifi/pi-host/dnsmasq/dnsmasq.conf /etc/dnsmasq.conf
sudo systemctl restart dnsmasq
```

The board will renew its DHCP lease and get the static IP within 30 seconds (or power-cycle it). Confirm:

```bash
ping -c 3 192.168.42.11
```

You should get replies. If not, power-cycle the MCU board and try again.

---

## Step 8 — (Optional) Pin the BSSID for faster reconnect

By telling the MCU firmware the Pi's exact WiFi MAC, the boards skip scanning and reconnect faster after a reset (from ~4 s down to ~1 s).

Get the Pi's wlan0 MAC:

```bash
ip link show wlan0 | grep ether
# Example: link/ether dc:a6:32:12:34:56 brd ff:ff:ff:ff:ff:ff
```

**On your PC**, edit [esp-bridge/main/config.h](../esp-bridge/main/config.h):

```c
#define AP_BSSID     {0xdc, 0xa6, 0x32, 0x12, 0x34, 0x56}
#define AP_BSSID_SET true
```

Re-flash the MCU-side XIAO (BOOT + RESET + Upload). Only bother with this after everything is working.

---

## Step 9 — Add to Klipper

The config snippet is already in this repo at [klipper-config/mcu-wifi.cfg](../klipper-config/mcu-wifi.cfg). Copy or symlink it into your Klipper config directory, then add to `printer.cfg`:

```ini
[include mcu-wifi.cfg]
```

Or add the socket reference directly:

```ini
[mcu toolhead]
serial: socket://192.168.42.11:23
restart_method: command
```

Restart Klipper and check the log:

```bash
sudo journalctl -u klipper -f
# Look for: MCU 'toolhead' is now ready
```

---

## Troubleshooting

### wlan0 disappears after reboot
The SPI module is not auto-loading. Re-run `./setup-esp-hosted.sh` — it rebuilds and reloads the module.

### hostapd: "Failed to set beacon parameters"
`wlan0` exists but ESP-Hosted is not fully initialised. Check:
```bash
dmesg | grep -E 'esp|spi' | tail -20
```
Look for SPI timeout or GPIO errors — usually a wiring issue.

### MCU board connects but gets a dynamic IP, not 192.168.42.11
The static lease in `dnsmasq.conf` doesn't match its MAC. Double-check Step 7 — MACs are case-sensitive in dnsmasq.

### Klipper says "Unable to connect" / socket timeout
```bash
ping -c 3 192.168.42.11      # must reply first
telnet 192.168.42.11 23      # should open a connection (close with Ctrl+])
```
If ping works but telnet doesn't, the bridge firmware is not running — check serial monitor at 115200 baud (connect XIAO USB to PC while MCU board is powered).

### Static IP on wlan0 lost after reboot
```bash
sudo nmcli con up wlan0-static
```
If that fails, re-run `./setup-ap.sh`.

### Everything worked once but broke after a Pi OS update
A kernel update can invalidate the compiled `esp32_spi` module. Re-run `./setup-esp-hosted.sh` to rebuild it against the new kernel headers.

---

## Maintenance

- **Source of truth for configs:** edit files under `pi-host/` in this repo on your PC, then copy to the Pi and restart the relevant service.
- **Adding more MCU boards:** repeat Steps 6–7 for each board, using `.12`, `.13` etc. as the static IP. Flash each XIAO with a different `STATIC_IP` in `config.h`.
- **Klipper firmware updates on the STM32:** use the `restart_method: command` in `mcu-wifi.cfg` — Klipper will reset the STM32 via the bridge's GPIO control of NRST.
