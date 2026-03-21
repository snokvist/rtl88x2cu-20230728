#!/bin/sh
#
# coop-rx-start.sh — Start cooperative RX diversity (OpenIPC/BusyBox)
#
# Usage: coop-rx-start.sh [SSID] [PSK]
#   Defaults: SSID=waybeam-03, PSK=waybeam-03
#
# Designed for OpenIPC sbc-groundstations image (Radxa Zero 3):
#   - BusyBox shell (no bash arrays)
#   - udhcpc (no dhclient/dhcpcd)
#   - No NetworkManager
#   - Module at /lib/modules/$(uname -r)/extra/88x2cu.ko
#
# Sequence:
#   1. Unload driver, unbind helper USB
#   2. Load driver (only primary visible)
#   3. Connect primary via wpa_supplicant, get DHCP
#   4. Rebind helper USB
#   5. Pair + bind cooperative session

set -e

SSID="${1:-waybeam-03}"
PSK="${2:-waybeam-03}"
MODULE="/lib/modules/$(uname -r)/extra/88x2cu.ko"
WAIT_ASSOC=15

info() { echo "[+] $*"; }
warn() { echo "[!] $*"; }
fail() { echo "[-] $*"; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "Must run as root"
[ -f "$MODULE" ] || fail "Module not found: $MODULE"

# --- Find RTL8822CU USB devices (0bda:c812) ---------------------------------

find_rtl_usb() {
    for dev in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
        [ -f "$dev/idVendor" ] || continue
        vid=$(cat "$dev/idVendor" 2>/dev/null)
        pid=$(cat "$dev/idProduct" 2>/dev/null)
        if [ "$vid" = "0bda" ] && [ "$pid" = "c812" ]; then
            basename "$dev"
        fi
    done
}

USB_DEVS=$(find_rtl_usb)
USB_COUNT=$(echo "$USB_DEVS" | wc -w)

if [ "$USB_COUNT" -lt 2 ]; then
    fail "Need 2 RTL8822CU USB devices, found $USB_COUNT: $USB_DEVS"
fi

USB_PRIMARY=$(echo "$USB_DEVS" | head -1)
USB_HELPER=$(echo "$USB_DEVS" | tail -1)
info "USB devices: primary=$USB_PRIMARY helper=$USB_HELPER"

# --- Phase 1: Clean teardown ------------------------------------------------

killall wpa_supplicant 2>/dev/null || true
sleep 1

if lsmod | grep -q "^88x2cu"; then
    info "Unloading driver..."
    for iface in /sys/class/net/wl*; do
        [ -d "$iface" ] && ip link set "$(basename "$iface")" down 2>/dev/null
    done
    sleep 1
    rmmod 88x2cu 2>/dev/null || warn "rmmod failed"
    sleep 2
fi

info "Unbinding helper USB ($USB_HELPER)..."
if [ -d "/sys/bus/usb/devices/$USB_HELPER/driver" ]; then
    echo "$USB_HELPER" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null || true
fi
sleep 1

# --- Phase 2: Load driver (only primary visible) ----------------------------

info "Loading driver..."
insmod "$MODULE" rtw_cooperative_rx=1 || fail "Failed to load module"
sleep 3

PRIMARY=""
for dev in /sys/class/net/wl*; do
    [ -d "$dev" ] || continue
    name=$(basename "$dev")
    [ -d "$dev/coop_rx" ] && PRIMARY="$name" && break
done
[ -n "$PRIMARY" ] || fail "No interface found after driver load"
info "Primary interface: $PRIMARY"

# --- Phase 3: Connect primary -----------------------------------------------

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
info "Connecting to $SSID..."
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

FREQ=$(iw dev "$PRIMARY" link 2>/dev/null | grep freq | awk '{print $2}')
info "Connected on ${FREQ} MHz"

# --- Phase 4: DHCP ----------------------------------------------------------

info "Getting DHCP lease..."
udhcpc -i "$PRIMARY" -n -q -t 5 2>/dev/null || warn "DHCP failed"
IP=$(ip -4 addr show "$PRIMARY" 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}')
info "Primary IP: ${IP:-none}"

# --- Phase 5: Rebind helper USB ---------------------------------------------

info "Rebinding helper USB ($USB_HELPER)..."
echo "$USB_HELPER" > /sys/bus/usb/drivers/usb/bind 2>/dev/null || warn "USB rebind failed"

HELPER=""
i=0
while [ $i -lt 10 ]; do
    sleep 1
    for dev in /sys/class/net/wl*; do
        name=$(basename "$dev")
        [ "$name" = "$PRIMARY" ] && continue
        [ -d "$dev/coop_rx" ] && HELPER="$name" && break 2
    done
    i=$((i + 1))
done

if [ -z "$HELPER" ]; then
    warn "Helper interface not found"
else
    info "Helper interface: $HELPER"
fi

# --- Phase 6: Pair + bind ---------------------------------------------------

if [ -n "$HELPER" ]; then
    ip link set "$HELPER" up

    info "Pairing helper..."
    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" \
        || fail "Pairing failed"

    info "Binding session..."
    echo "1" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" \
        || fail "Bind failed"

    BOUND_CH=$(cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null | grep bound_channel | awk '{print $2}')
    info "Helper in monitor mode on ch$BOUND_CH"
fi

# --- Verify ------------------------------------------------------------------

GW=$(ip route show dev "$PRIMARY" 2>/dev/null | grep default | awk '{print $3}')
GW="${GW:-10.6.0.1}"
if ping -c 2 -W 2 "$GW" >/dev/null 2>&1; then
    info "Ping to $GW OK"
else
    warn "Ping to $GW failed"
fi

echo ""
info "=== Cooperative RX Status ==="
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null
echo ""
info "Commands:"
echo "  Monitor:  python3 $(dirname "$0")/coop-rx-monitor.py"
echo "  Stop:     $(dirname "$0")/coop-rx-stop.sh"
