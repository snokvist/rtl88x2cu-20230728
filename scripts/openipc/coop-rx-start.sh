#!/bin/sh
#
# coop-rx-start.sh — Start cooperative RX diversity (OpenIPC/BusyBox)
#
# Usage: coop-rx-start.sh [SSID] [PSK]
#   Defaults: SSID=waybeam-03, PSK=waybeam-03
#
# Designed for OpenIPC sbc-groundstations image (Radxa Zero 3).
# Safe to run over SSH — does NOT rmmod/insmod the driver.
# Expects the module already loaded with rtw_cooperative_rx=1
# (set via /etc/modprobe.d/88x2cu.conf).
#
# If the primary is already connected (waybeam_hub manages WiFi),
# the script skips wpa_supplicant and just pairs + binds the helper.

set -e

SSID="${1:-waybeam-03}"
PSK="${2:-waybeam-03}"
WAIT_ASSOC=15

info() { echo "[+] $*"; }
warn() { echo "[!] $*"; }
fail() { echo "[-] $*"; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "Must run as root"

# --- Check module loaded with coop_rx enabled --------------------------------

if ! lsmod | grep -q "^88x2cu"; then
    fail "88x2cu module not loaded. Load with: insmod 88x2cu.ko rtw_cooperative_rx=1"
fi

COOP_PARAM=$(cat /sys/module/88x2cu/parameters/rtw_cooperative_rx 2>/dev/null)
if [ "$COOP_PARAM" != "1" ]; then
    fail "Module loaded without rtw_cooperative_rx=1. Add to /etc/modprobe.d/88x2cu.conf and reload"
fi

# --- Find interfaces ---------------------------------------------------------

PRIMARY=""
HELPER=""
IFACES=""

for dev in /sys/class/net/wl*; do
    [ -d "$dev/coop_rx" ] || continue
    name=$(basename "$dev")
    IFACES="$IFACES $name"
done

IFACE_COUNT=$(echo $IFACES | wc -w)
if [ "$IFACE_COUNT" -lt 2 ]; then
    fail "Need 2 RTL8822CU interfaces, found $IFACE_COUNT:$IFACES"
fi

# --- Check if already active -------------------------------------------------

for name in $IFACES; do
    role=$(cat "/sys/class/net/$name/coop_rx/coop_rx_role" 2>/dev/null)
    if [ "$role" = "primary" ]; then
        state=$(cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null | grep "^state:" | awk '{print $2}')
        if [ "$state" = "3" ]; then
            info "Cooperative RX already ACTIVE on $name"
            cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null | grep -E "state|helpers|accepted|kern_crypto"
            exit 0
        fi
    fi
done

# --- Determine primary (connected) vs helper ---------------------------------

# Check if any interface is already associated (waybeam_hub may have connected)
for name in $IFACES; do
    if iw dev "$name" link 2>/dev/null | grep -q "Connected"; then
        PRIMARY="$name"
        break
    fi
done

# If none connected, pick first and connect
if [ -z "$PRIMARY" ]; then
    PRIMARY=$(echo $IFACES | awk '{print $1}')

    info "Connecting $PRIMARY to $SSID..."
    killall wpa_supplicant 2>/dev/null || true
    sleep 1

    cat > /tmp/coop_wpa.conf <<EOF
network={
    ssid="$SSID"
    psk="$PSK"
    key_mgmt=WPA-PSK
    proto=RSN
    pairwise=CCMP
    group=CCMP
}
EOF

    rm -f "/var/run/wpa_supplicant/$PRIMARY" 2>/dev/null
    mkdir -p /var/run/wpa_supplicant
    ip link set "$PRIMARY" up
    wpa_supplicant -B -D nl80211 -i "$PRIMARY" -c /tmp/coop_wpa.conf

    i=0
    while [ $i -lt $WAIT_ASSOC ]; do
        iw dev "$PRIMARY" link 2>/dev/null | grep -q "Connected" && break
        i=$((i + 1))
        sleep 1
    done

    if ! iw dev "$PRIMARY" link 2>/dev/null | grep -q "Connected"; then
        fail "Association failed within ${WAIT_ASSOC}s"
    fi

    info "Getting DHCP lease..."
    udhcpc -i "$PRIMARY" -n -q -t 5 2>/dev/null || warn "DHCP failed"
else
    info "Primary $PRIMARY already connected"
fi

FREQ=$(iw dev "$PRIMARY" link 2>/dev/null | grep freq | awk '{print $2}')
IP=$(ip -4 addr show "$PRIMARY" 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}')
info "Primary: $PRIMARY on ${FREQ:-?} MHz, IP: ${IP:-none}"

# --- Find helper (the other interface) ---------------------------------------

for name in $IFACES; do
    [ "$name" = "$PRIMARY" ] && continue
    HELPER="$name"
    break
done

[ -n "$HELPER" ] || fail "No helper interface found"
info "Helper: $HELPER"

# --- Pair + bind -------------------------------------------------------------

ip link set "$HELPER" up

info "Pairing helper..."
echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" \
    || fail "Pairing failed"

info "Binding session..."
echo "1" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" \
    || fail "Bind failed"

sleep 1

# --- Verify ------------------------------------------------------------------

BOUND_CH=$(cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null | grep bound_channel | awk '{print $2}')
info "Helper in monitor mode on ch${BOUND_CH:-?}"

GW=$(ip route show dev "$PRIMARY" 2>/dev/null | grep default | awk '{print $3}')
if [ -n "$GW" ]; then
    if ping -c 2 -W 2 "$GW" >/dev/null 2>&1; then
        info "Ping to $GW OK"
    else
        warn "Ping to $GW failed"
    fi
fi

echo ""
info "=== Cooperative RX Status ==="
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null
echo ""
info "Monitor: python3 $(dirname "$0")/coop-rx-monitor.py"
info "Stop:    $(dirname "$0")/coop-rx-stop.sh"
