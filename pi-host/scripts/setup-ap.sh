#!/bin/bash
# Pi 5 AP setup — run once after ESP-Hosted is working
# Tested on Pi OS Bookworm (NetworkManager)

set -e

echo "=== Installing hostapd + dnsmasq ==="
sudo apt update
sudo apt install -y hostapd dnsmasq
sudo systemctl unmask hostapd

echo "=== Configuring static IP for wlan0 via NetworkManager ==="
# Pi OS Bookworm uses NetworkManager, not dhcpcd
# Tell NetworkManager to leave wlan0 alone — hostapd manages it directly
sudo nmcli dev set wlan0 managed no || true

# Assign static IP via ip command (hostapd needs the interface up with an IP)
sudo ip addr flush dev wlan0 || true
sudo ip addr add 192.168.42.1/24 dev wlan0
sudo ip link set wlan0 up

echo "=== Copying configs ==="
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo cp "$SCRIPT_DIR/../hostapd/hostapd.conf" /etc/hostapd/hostapd.conf
sudo cp "$SCRIPT_DIR/../dnsmasq/dnsmasq.conf" /etc/dnsmasq.conf

echo "=== Applying TCP tuning (disable Nagle, small buffers) ==="
sudo cp "$SCRIPT_DIR/../sysctl/sysctl-klipper.conf" /etc/sysctl.d/99-klipper.conf
sudo sysctl --system

echo "=== Making static IP persistent across reboots ==="
# Drop a NetworkManager connection profile that holds wlan0 at 192.168.42.1
# and keeps NM from handing it to dhcpcd or changing it on boot.
sudo nmcli con delete wlan0-static 2>/dev/null || true
sudo nmcli con add \
    type wifi \
    ifname wlan0 \
    con-name wlan0-static \
    ssid klipper \
    -- \
    wifi-sec.key-mgmt none \
    ipv4.method manual \
    ipv4.addresses 192.168.42.1/24 \
    ipv6.method disabled \
    connection.autoconnect no
# autoconnect=no because hostapd owns the interface; NM just holds the IP config
sudo nmcli con modify wlan0-static 802-11-wireless.mode ap

echo "=== Enabling services ==="
sudo systemctl enable hostapd dnsmasq
sudo systemctl restart hostapd dnsmasq

echo ""
echo "Done. Check 'sudo systemctl status hostapd' to verify."
echo "SSID: klipper  |  Password: klipper"
