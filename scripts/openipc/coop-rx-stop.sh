#!/bin/sh
#
# coop-rx-stop.sh — Tear down cooperative RX (OpenIPC/BusyBox)
#

set -e

[ "$(id -u)" -eq 0 ] || { echo "Must run as root"; exit 1; }

echo "[+] Unbinding cooperative session..."
for dev in /sys/class/net/wl*; do
    [ -d "$dev/coop_rx" ] || continue
    echo 0 > "$dev/coop_rx/coop_rx_bind" 2>/dev/null || true
done

echo "[+] Stopping wpa_supplicant..."
killall wpa_supplicant 2>/dev/null || true
sleep 1

echo "[+] Final stats:"
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null || echo "  (not available)"

echo "[+] Bringing interfaces down..."
for dev in /sys/class/net/wl*; do
    [ -d "$dev" ] || continue
    name=$(basename "$dev")
    ip link set "$name" down 2>/dev/null || true
done

echo "[+] Releasing DHCP..."
killall udhcpc 2>/dev/null || true

rm -f /tmp/coop_wpa.conf

echo "[+] Done. Driver still loaded — use 'rmmod 88x2cu' to unload."
