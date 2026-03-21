#!/bin/bash
#
# coop-rx-ap-test-stop.sh — Tear down AP mode cooperative RX test
#
# Restores the remote device's hostapd and cleans up local state.
#
# Usage: sudo ./coop-rx-ap-test-stop.sh [REMOTE_HOST]
#

set -euo pipefail

REMOTE_HOST="${1:-192.168.1.13}"

info() { echo -e "\033[0;32m[+]\033[0m $*"; }
warn() { echo -e "\033[1;33m[!]\033[0m $*"; }

[[ $EUID -eq 0 ]] || { echo "Must run as root"; exit 1; }

info "Final cooperative RX stats:"
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null || echo "  (not available)"

info "Stopping local hostapd..."
killall hostapd 2>/dev/null || true

info "Stopping local wpa_supplicant..."
pkill wpa_supplicant 2>/dev/null || true

info "Unbinding cooperative session..."
for dev in /sys/class/net/wl*; do
    [ -f "$dev/coop_rx/coop_rx_bind" ] 2>/dev/null && \
        echo 0 > "$dev/coop_rx/coop_rx_bind" 2>/dev/null || true
done

info "Bringing local interfaces down..."
for dev in /sys/class/net/wl*; do
    [ -d "$dev/coop_rx" ] && ip link set "$(basename "$dev")" down 2>/dev/null || true
done

# Restore remote device
REMOTE_WIFI=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "ls /sys/class/net/ | grep wlan | head -1" 2>/dev/null || true)

if [ -n "$REMOTE_WIFI" ]; then
    info "Stopping remote wpa_supplicant..."
    ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "killall wpa_supplicant 2>/dev/null; ip addr flush dev $REMOTE_WIFI 2>/dev/null; ip link set $REMOTE_WIFI down 2>/dev/null" \
        2>/dev/null || warn "Remote cleanup failed"

    info "Restarting remote hostapd (waybeam-03)..."
    ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "hostapd -B -P /var/run/hostapd.wlan0.pid /tmp/hostapd.conf 2>/dev/null || hostapd /etc/hostapd.conf -B 2>/dev/null" \
        2>/dev/null || warn "Remote hostapd restart failed — may need manual restart"
    sleep 2

    REMOTE_STATUS=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "iw dev $REMOTE_WIFI info 2>/dev/null | grep -E 'type|ssid'" 2>/dev/null || true)
    info "Remote status: $REMOTE_STATUS"
fi

info "Cleaning up temp files..."
rm -f /tmp/coop_rx_ap_test.conf /tmp/coop_ap_test_sta.conf

info "Restarting NetworkManager..."
systemctl start NetworkManager 2>/dev/null || true

info "Done. Run 'sudo rmmod 88x2cu' to unload driver."
