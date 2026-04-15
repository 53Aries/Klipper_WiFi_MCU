#!/bin/bash
# ESP-Hosted-FG setup for Pi 5 + ESP32-C5 over SPI
# Run this BEFORE setup-ap.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
DTBO_DEST="/boot/firmware/overlays/spidev-disabler.dtbo"
dtc -@ "$SCRIPT_DIR/spidev-disabler.dts" -O dtb -o /tmp/spidev-disabler.dtbo
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
#   spi-handshake=536 → BCM GPIO24 (physical pin 18) → C5 IO3
#   spi-dataready=539 → BCM GPIO27 (physical pin 13) → C5 IO4
# NOTE: BCM22 (pin 15) is permanently claimed by spi10 cs-gpios (2712_BOOT_CS_N).
#       BCM23 (pin 16) has a hardware pull-up (2712_BOOT_MISO) that holds the
#       line HIGH permanently, preventing the driver from seeing rising edges.

# Module parameters for Pi 5 wiring
MOD_PARAMS="resetpin=529 spi_bus=10 spi_cs=0 spi_mode=3 spi_handshake=536 spi_dataready=539 clockspeed=10"
MOD_NAME="esp32_spi"
KO_SRC="$HOME/esp-hosted/esp_hosted_fg/host/linux/host_driver/esp32/spi/esp32_spi.ko"

# Build the module via rpi_init.sh (it compiles and does a one-shot insmod)
./rpi_init.sh wifi=spi bt=- spi-mode=3 --skip-build-apps \
    spi-bus=10 spi-cs=0 resetpin=529 spi-handshake=536 spi-dataready=539 || {
    if [ "$REBOOT_NEEDED" -eq 1 ]; then
        echo ""
        echo "NOTE: Module built OK but SPI hardware is not active until after a reboot."
        echo "This is expected on the first run."
    else
        echo "ERROR: Module failed to load. Run: dmesg | tail -30"
        exit 1
    fi
}

echo "=== Installing module for persistent boot loading ==="
# Copy the compiled .ko into the kernel's extra modules directory
KVER="$(uname -r)"
EXTRA_DIR="/lib/modules/${KVER}/extra"
sudo mkdir -p "$EXTRA_DIR"
sudo cp "$KO_SRC" "$EXTRA_DIR/${MOD_NAME}.ko"
sudo depmod -a

# Set default module parameters so modprobe loads with our Pi 5 GPIO config
sudo tee /etc/modprobe.d/esp32-spi.conf > /dev/null <<EOF
# ESP-Hosted SPI module parameters for Pi 5 + XIAO ESP32-C5
options esp32_spi ${MOD_PARAMS}
EOF

# Tell the kernel to load the module at every boot
if ! grep -q "^esp32_spi" /etc/modules-load.d/esp32-spi.conf 2>/dev/null; then
    echo "esp32_spi" | sudo tee /etc/modules-load.d/esp32-spi.conf > /dev/null
fi

echo ""
echo "=== SPI GPIO pinout (Pi 5 physical pins → ESP32-C5) ==="
echo "  Pin 19 (GPIO10 SPI0 MOSI) → IO7"
echo "  Pin 21 (GPIO9  SPI0 MISO) → IO2"
echo "  Pin 23 (GPIO11 SPI0 SCLK) → IO6"
echo "  Pin 24 (GPIO8  SPI0 CE0)  → IO10"
echo "  Pin 9  (GND)              → GND"
echo "  Pin 13 (GPIO27 / #539)    → IO4  (Data Ready)"
echo "  Pin 18 (GPIO24 / #536)    → IO3  (Handshake)"
echo "  Pin 11 (GPIO17 / #529)    → RST"
echo ""
echo "=== Done ==="
echo "The esp32_spi module will auto-load on every boot with the correct parameters."
echo "Reboot now:  sudo reboot"
echo "After reboot, verify:  lsmod | grep esp && cat /proc/interrupts | grep ESP"
