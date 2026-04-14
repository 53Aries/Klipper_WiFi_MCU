#!/bin/bash
# ESP-Hosted-FG setup for Pi 5 + ESP32-C5 over SPI
# Run this BEFORE setup-ap.sh

set -e

echo "=== Installing build dependencies ==="
sudo apt update
sudo apt install -y git build-essential "linux-headers-$(uname -r)"

echo "=== Enabling SPI in /boot/firmware/config.txt ==="
CONFIG=/boot/firmware/config.txt
if ! grep -q "dtparam=spi=on" "$CONFIG"; then
    echo "dtparam=spi=on" | sudo tee -a "$CONFIG"
fi
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

echo "=== Building kernel module (SPI, ESP32-C5) ==="
# rpi_init.sh lives in host_control/, not directly in host/linux/
HOST_DIR="$HOME/esp-hosted/esp_hosted_fg/host/linux/host_control"
if [ ! -f "$HOST_DIR/rpi_init.sh" ]; then
    echo "ERROR: rpi_init.sh not found at $HOST_DIR"
    echo "Check that esp-hosted cloned correctly and the path is still valid."
    exit 1
fi
cd "$HOST_DIR"

# Release spidev's hold on spi10.0 so esp32_spi can claim it.
# On Pi 5 the RP1 SPI0 appears as bus 10; spidev grabs it first.
if [ -e /sys/bus/spi/drivers/spidev/spi10.0 ]; then
    echo "=== Releasing spidev from spi10.0 ==="
    echo spi10.0 | sudo tee /sys/bus/spi/drivers/spidev/unbind > /dev/null
fi

# GPIO numbers below are Pi 5 Linux GPIO numbers (BCM + 512 offset for RP1).
# These match our wiring:
#   resetpin=529      → BCM GPIO17 (physical pin 11) → C5 RST
#   spi-handshake=534 → BCM GPIO22 (physical pin 15) → C5 IO3
#   spi-dataready=539 → BCM GPIO27 (physical pin 13) → C5 IO4
./rpi_init.sh wifi=spi bt=- spi-mode=3 --skip-build-apps \
    spi-bus=10 resetpin=529 spi-handshake=534 spi-dataready=539 || {
    if [ ! -e /dev/spidev10.0 ]; then
        echo ""
        echo "NOTE: Module built OK but could not be loaded yet — SPI hardware"
        echo "is not active until after a reboot. This is expected on first run."
    else
        echo "ERROR: Failed to insert module even though SPI is available."
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
