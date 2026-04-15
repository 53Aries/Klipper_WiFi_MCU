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

# Auto-detect the RP1 GPIO base offset.
# On Bookworm this was 512, on Trixie it moved to 569.
# The RP1 chip is labeled "pinctrl-rp1" and has 54 lines.
RP1_BASE=""
for chip in /sys/class/gpio/gpiochip*/; do
    label=$(cat "$chip/label" 2>/dev/null)
    ngpio=$(cat "$chip/ngpio" 2>/dev/null)
    if [ "$label" = "pinctrl-rp1" ] && [ "$ngpio" = "54" ]; then
        RP1_BASE=$(cat "$chip/base")
        break
    fi
done
if [ -z "$RP1_BASE" ]; then
    echo "ERROR: Could not find RP1 GPIO chip (pinctrl-rp1). Is this a Pi 5?"
    exit 1
fi
echo "=== Detected RP1 GPIO base: $RP1_BASE ==="

# BCM GPIO numbers for our wiring (add RP1_BASE to get Linux GPIO number):
#   BCM17 (pin 11) → RST          BCM24 (pin 18) → Handshake (IO4 / FSPIHD)
#   BCM27 (pin 13) → DataReady (IO5 / FSPIWP)
RESETPIN=$((RP1_BASE + 17))
HANDSHAKE=$((RP1_BASE + 24))
DATAREADY=$((RP1_BASE + 27))

echo "  resetpin=$RESETPIN  handshake=$HANDSHAKE  dataready=$DATAREADY"

# Module parameters for Pi 5 wiring
MOD_PARAMS="resetpin=${RESETPIN} spi_bus=10 spi_cs=0 spi_mode=3 spi_handshake=${HANDSHAKE} spi_dataready=${DATAREADY} clockspeed=10"
MOD_NAME="esp32_spi"
KO_DIR="$HOME/esp-hosted/esp_hosted_fg/host/linux/host_driver/esp32"

# Build the module via rpi_init.sh (it compiles and does a one-shot insmod)
./rpi_init.sh wifi=spi bt=- spi-mode=3 --skip-build-apps \
    spi-bus=10 spi-cs=0 resetpin=$RESETPIN spi-handshake=$HANDSHAKE spi-dataready=$DATAREADY || {
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
# Find the compiled .ko — build may put it in esp32/ or esp32/spi/
KO_SRC=$(find "$KO_DIR" -name "${MOD_NAME}.ko" -print -quit 2>/dev/null)
if [ -z "$KO_SRC" ]; then
    echo "ERROR: Could not find ${MOD_NAME}.ko under $KO_DIR"
    echo "Check build output above for errors."
    exit 1
fi
echo "Found module: $KO_SRC"
# Copy the compiled .ko into the kernel's extra modules directory
KVER="$(uname -r)"
EXTRA_DIR="/lib/modules/${KVER}/extra"
sudo mkdir -p "$EXTRA_DIR"
sudo cp "$KO_SRC" "$EXTRA_DIR/${MOD_NAME}.ko"
sudo depmod -a

# Set default module parameters so modprobe loads with our Pi 5 GPIO config
sudo tee /etc/modprobe.d/esp32-spi.conf > /dev/null <<EOF
# ESP-Hosted SPI module parameters for Pi 5 + ESP32-C5
# RP1 GPIO base auto-detected as $RP1_BASE (BCM + $RP1_BASE)
options esp32_spi ${MOD_PARAMS}
EOF

# Tell the kernel to load the module at every boot
if ! grep -q "^esp32_spi" /etc/modules-load.d/esp32-spi.conf 2>/dev/null; then
    echo "esp32_spi" | sudo tee /etc/modules-load.d/esp32-spi.conf > /dev/null
fi

echo ""
echo "=== SPI GPIO pinout (Pi 5 physical pins → ESP32-C5) ==="
echo "  RP1 GPIO base: $RP1_BASE"
echo "  Pin 19 (BCM10 SPI0 MOSI) → GPIO7"
echo "  Pin 21 (BCM9  SPI0 MISO) → GPIO2"
echo "  Pin 23 (BCM11 SPI0 SCLK) → GPIO6"
echo "  Pin 24 (BCM8  SPI0 CE0)  → GPIO10"
echo "  Pin 9  (GND)             → GND"
echo "  Pin 13 (BCM27 / #$DATAREADY)  → GPIO5  (Data Ready)"
echo "  Pin 18 (BCM24 / #$HANDSHAKE)  → GPIO4  (Handshake)"
echo "  Pin 11 (BCM17 / #$RESETPIN)   → RST"
echo ""
echo "=== Done ==="
echo "The esp32_spi module will auto-load on every boot with the correct parameters."
echo "Reboot now:  sudo reboot"
echo "After reboot, verify:  lsmod | grep esp && cat /proc/interrupts | grep ESP"
