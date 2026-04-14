#!/bin/bash
# ESP-Hosted-FG setup for Pi 5 + ESP32-C5 over SPI
# Run this BEFORE setup-ap.sh

set -e

echo "=== Installing build dependencies ==="
sudo apt update
sudo apt install -y git build-essential device-tree-compiler "linux-headers-$(uname -r)"

echo "=== Enabling SPI in /boot/firmware/config.txt ==="
CONFIG=/boot/firmware/config.txt
# Pi 5 has SPI0 and spidev@0 active by default — dtparam=spi=on is not needed
# and is removed if a previous script version added it.
sudo sed -i '/^dtparam=spi=on/d' "$CONFIG"
sudo sed -i '/^dtoverlay=spi0-0cs/d' "$CONFIG"
REBOOT_NEEDED=0
# Disable Bluetooth to free up UART if needed
if ! grep -q "dtoverlay=disable-bt" "$CONFIG"; then
    echo "dtoverlay=disable-bt" | sudo tee -a "$CONFIG"
fi

echo "=== Cloning ESP-Hosted ==="
cd ~
if [ ! -d esp-hosted ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/espressif/esp-hosted.git
else
    git -C esp-hosted pull --ff-only
    git -C esp-hosted submodule update --init --recursive
fi

echo "=== Installing spidev-disabler boot overlay ==="
# Pi 5 has spidev bound to spi10 (SPI0 on the 40-pin header) by default.
# nospi10 kills the whole bus controller — we need to disable only the spidev
# child device, leaving the spi10 bus alive for esp32_spi to claim.
# The base DT exports &spidev10 (confirmed via /proc/device-tree/__symbols__).
# Remove any leftover nospi10 entries from a previous script version.
sudo sed -i '/^dtoverlay=nospi10/d' "$CONFIG"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DTBO_DEST="/boot/firmware/overlays/spidev-disabler.dtbo"
dtc "$SCRIPT_DIR/spidev-disabler.dts" -O dtb -o /tmp/spidev-disabler.dtbo
if ! cmp -s /tmp/spidev-disabler.dtbo "$DTBO_DEST" 2>/dev/null; then
    sudo cp /tmp/spidev-disabler.dtbo "$DTBO_DEST"
    REBOOT_NEEDED=1
fi
if ! grep -q "dtoverlay=spidev-disabler" "$CONFIG"; then
    echo "dtoverlay=spidev-disabler" | sudo tee -a "$CONFIG"
    REBOOT_NEEDED=1
fi

echo "=== Building kernel module (SPI, ESP32-C5) ==="
# rpi_init.sh lives in host_control/, not directly in host/linux/
HOST_DIR="$HOME/esp-hosted/esp_hosted_fg/host/linux/host_control"
if [ ! -f "$HOST_DIR/rpi_init.sh" ]; then
    echo "ERROR: rpi_init.sh not found at $HOST_DIR"
    echo "Check that esp-hosted cloned correctly and the path is still valid."
    exit 1
fi
cd "$HOST_DIR"

# GPIO numbers below are Pi 5 Linux GPIO numbers (BCM + 512 offset for RP1).
# These match our wiring:
#   resetpin=529      → BCM GPIO17 (physical pin 11) → C5 RST
#   spi-handshake=534 → BCM GPIO22 (physical pin 15) → C5 IO3
#   spi-dataready=539 → BCM GPIO27 (physical pin 13) → C5 IO4
./rpi_init.sh wifi=spi bt=- spi-mode=3 --skip-build-apps \
    spi-bus=10 spi-cs=0 resetpin=529 spi-handshake=534 spi-dataready=539 || {
    if [ "$REBOOT_NEEDED" -eq 1 ]; then
        echo ""
        echo "NOTE: Module built OK but SPI hardware is not active until after a reboot."
        echo "This is expected on the first run."
    else
        echo "ERROR: Module failed to load. Run: dmesg | tail -30"
        exit 1
    fi
}

echo ""
echo "=== SPI GPIO pinout (Pi 5 physical pins → ESP32-C5) ==="
echo "  Pin 19 (GPIO10 SPI0 MOSI) → IO7"
echo "  Pin 21 (GPIO9  SPI0 MISO) → IO2"
echo "  Pin 23 (GPIO11 SPI0 SCLK) → IO6"
echo "  Pin 24 (GPIO8  SPI0 CE0)  → IO10"
echo "  Pin 25 (GND)              → GND"
echo "  Pin 13 (GPIO27 / #539)    → IO4  (Data Ready)"
echo "  Pin 15 (GPIO22 / #534)    → IO3  (Handshake)"
echo "  Pin 11 (GPIO17 / #529)    → RST"
echo ""
echo "Reboot required for SPI overlay to take effect."
echo "After reboot, run: lsmod | grep esp — you should see esp32_spi"
