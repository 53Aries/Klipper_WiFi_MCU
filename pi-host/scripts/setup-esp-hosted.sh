#!/bin/bash
# ESP-Hosted-FG setup for Pi 5 + ESP32-C5 over SPI
# Run this BEFORE setup-ap.sh

set -e

echo "=== Installing build dependencies ==="
sudo apt update
sudo apt install -y git build-essential device-tree-compiler "linux-headers-$(uname -r)"

echo "=== Enabling SPI in /boot/firmware/config.txt ==="
CONFIG=/boot/firmware/config.txt
# Remove old spi0-0cs overlay if present (previous script version).
sudo sed -i '/^dtoverlay=spi0-0cs/d' "$CONFIG"
REBOOT_NEEDED=0
# dtparam=spi=on enables SPI0 with CS GPIOs (GPIO8=CE0, GPIO7=CE1).
# We also install a boot-time overlay (spidev-disabler) below that prevents
# the spidev driver from claiming CE0, leaving it free for esp32_spi.
if ! grep -q "dtparam=spi=on" "$CONFIG"; then
    echo "dtparam=spi=on" | sudo tee -a "$CONFIG"
    REBOOT_NEEDED=1
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

echo "=== Installing spidev-disabler boot overlay ==="
# spidev_disabler.dts disables the spidev@0 DT node at boot time, preventing
# the spidev driver from binding to SPI0 CE0 before esp32_spi loads.
# Setting status=disabled at runtime (what rpi_init.sh does) has no effect
# because spidev has already bound; doing it at boot prevents binding entirely.
DTS_SRC="$HOME/esp-hosted/esp_hosted_fg/host/linux/host_control/spidev_disabler.dts"
DTBO_DEST="/boot/firmware/overlays/spidev-disabler.dtbo"
if [ ! -f "$DTBO_DEST" ]; then
    dtc "$DTS_SRC" -O dtb -o /tmp/spidev-disabler.dtbo 2>/dev/null
    sudo cp /tmp/spidev-disabler.dtbo "$DTBO_DEST"
fi
# dtoverlay=spidev-disabler must come after dtparam=spi=on in config.txt
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
