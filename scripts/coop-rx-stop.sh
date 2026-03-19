#!/bin/bash
#
# coop-rx-stop.sh — Tear down cooperative RX and clean up
#

set -e

[[ $EUID -eq 0 ]] || { echo "Must run as root"; exit 1; }

echo "[+] Unbinding cooperative session..."
for dev in /sys/class/net/wl*; do
    name=$(basename "$dev")
    if [ -f "$dev/coop_rx/coop_rx_bind" ] 2>/dev/null; then
        echo 0 > "$dev/coop_rx/coop_rx_bind" 2>/dev/null || true
    fi
done

echo "[+] Stopping wpa_supplicant instances..."
pkill wpa_supplicant 2>/dev/null || true
sleep 1

echo "[+] Final cooperative RX stats:"
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null || echo "  (not available)"

echo "[+] Bringing interfaces down..."
for dev in /sys/class/net/wl*; do
    name=$(basename "$dev")
    if [ -f "$dev/coop_rx/coop_rx_role" ] 2>/dev/null; then
        role=$(cat "$dev/coop_rx/coop_rx_role" 2>/dev/null)
        echo "  $name: role=$role"
        ip link set "$name" down 2>/dev/null || true
    fi
done

echo "[+] Releasing DHCP leases..."
dhcpcd --release 2>/dev/null || true

echo "[+] Cleaning up temp files..."
rm -f /tmp/coop_rx_primary.conf /tmp/coop_rx_helper.conf

NM_RESTART="${COOP_RX_RESTART_NM:-0}"
if [[ "$NM_RESTART" == "1" ]] || [[ "$1" == "--restart-nm" ]]; then
    echo "[+] Restarting NetworkManager..."
    systemctl restart NetworkManager 2>/dev/null || true
fi

echo "[+] Done. Driver still loaded — use 'rmmod 88x2cu' to unload."
