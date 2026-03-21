#!/bin/bash
#
# coop-rx-start.sh — Start cooperative RX diversity with two RTL8822CU adapters
#
# Usage: sudo ./coop-rx-start.sh [SSID] [PSK]
#   Defaults: SSID=waybeam-03, PSK=waybeam-03
#
# Both adapters can stay plugged in. The script uses USB unbind/rebind
# to isolate the helper while the primary connects, avoiding the
# cfg80211 registration conflict that causes auth timeouts.
#
# Sequence (designed to avoid USB/driver hangs):
#   1. Unload driver (if loaded) — no active I/O
#   2. USB-unbind helper — safe because no driver is using it
#   3. Load driver — only sees the primary adapter
#   4. Connect primary, get DHCP
#   5. USB-rebind helper — driver picks it up
#   6. Set helper to monitor mode, pair, bind
#

set -e

SSID="${1:-waybeam-03}"
PSK="${2:-waybeam-03}"
MODULE_PATH="$(cd "$(dirname "$0")/.." && pwd)/88x2cu.ko"
WAIT_ASSOC=20

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[+]${NC} $*"; }
warn()  { echo -e "${YLW}[!]${NC} $*"; }
fail()  { echo -e "${RED}[-]${NC} $*"; exit 1; }

[[ $EUID -eq 0 ]] || fail "Must run as root"

# --- Find RTL8822CU USB devices ------------------------------------------------

find_rtl_usb_devices() {
    local devices=()
    for dev in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
        [ -f "$dev/idVendor" ] || continue
        local vid=$(cat "$dev/idVendor" 2>/dev/null)
        local pid=$(cat "$dev/idProduct" 2>/dev/null)
        if [[ "$vid" == "0bda" && "$pid" == "c812" ]]; then
            devices+=("$(basename "$dev")")
        fi
    done
    echo "${devices[@]}"
}

USB_DEVICES=($(find_rtl_usb_devices))
if [[ ${#USB_DEVICES[@]} -lt 2 ]]; then
    fail "Need 2 RTL8822CU USB devices (0bda:c812), found ${#USB_DEVICES[@]}: ${USB_DEVICES[*]}"
fi
USB_PRIMARY="${USB_DEVICES[0]}"
USB_HELPER="${USB_DEVICES[1]}"
info "USB devices: primary=$USB_PRIMARY helper=$USB_HELPER"

# --- Phase 1: Clean teardown (safe order) --------------------------------------

info "Stopping NetworkManager..."
systemctl stop NetworkManager 2>/dev/null || true

info "Killing wpa_supplicant..."
pkill wpa_supplicant 2>/dev/null || true
sleep 1

# Unload driver FIRST — ensures no active I/O on any USB device
if lsmod | grep -q "^88x2cu"; then
    info "Unloading driver (ensures safe USB unbind)..."
    for iface in /sys/class/net/wl*; do
        ip link set "$(basename "$iface")" down 2>/dev/null
    done
    sleep 1
    rmmod 88x2cu 2>/dev/null || warn "rmmod failed — may need reboot"
    sleep 2
fi

# Now unbind helper USB — safe because driver is unloaded
info "Unbinding helper USB ($USB_HELPER)..."
if [ -d "/sys/bus/usb/devices/$USB_HELPER/driver" ]; then
    echo "$USB_HELPER" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null || true
fi
sleep 1

# --- Phase 2: Load driver with only primary visible ---------------------------

info "Loading driver (only primary adapter visible)..."
insmod "$MODULE_PATH" rtw_cooperative_rx=1 || fail "Failed to load module"
sleep 3

# Find primary interface
PRIMARY=""
for dev in /sys/class/net/wl*; do
    [ -d "$dev" ] || continue
    name=$(basename "$dev")
    [ -d "$dev/coop_rx" ] && PRIMARY="$name" && break
done
[[ -n "$PRIMARY" ]] || fail "No RTL8822CU interface found after driver load"
info "Primary interface: $PRIMARY"

# --- Phase 3: Connect primary -------------------------------------------------

WPA_CONF="/tmp/coop_rx_primary.conf"
cat > "$WPA_CONF" <<EOF
ctrl_interface=/var/run/wpa_supplicant
ctrl_interface_group=0

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

info "Connecting primary ($PRIMARY) to $SSID..."
ip link set "$PRIMARY" up
wpa_supplicant -B -D nl80211 -i "$PRIMARY" -c "$WPA_CONF"

for i in $(seq 1 $WAIT_ASSOC); do
    iw dev "$PRIMARY" link 2>/dev/null | grep -q "Connected" && break
    sleep 1
done

if ! iw dev "$PRIMARY" link 2>/dev/null | grep -q "Connected"; then
    fail "Primary failed to associate within ${WAIT_ASSOC}s"
fi

FREQ=$(iw dev "$PRIMARY" link 2>/dev/null | grep freq | awk '{print $2}')
info "Primary connected on ${FREQ} MHz"

# --- Phase 4: DHCP ------------------------------------------------------------

info "Getting DHCP lease..."
dhcpcd -1 -4 "$PRIMARY" 2>/dev/null || warn "DHCP failed — configure IP manually"
IP=$(ip -4 addr show "$PRIMARY" 2>/dev/null | grep -oP 'inet \K[0-9.]+')
info "Primary IP: ${IP:-none}"

# --- Phase 5: Rebind helper USB -----------------------------------------------

info "Rebinding helper USB ($USB_HELPER)..."
echo "$USB_HELPER" > /sys/bus/usb/drivers/usb/bind 2>/dev/null || warn "USB rebind failed"

# Wait for interface to appear
HELPER=""
for i in $(seq 1 10); do
    sleep 1
    for dev in /sys/class/net/wl*; do
        name=$(basename "$dev")
        [[ "$name" == "$PRIMARY" ]] && continue
        [ -d "$dev/coop_rx" ] && HELPER="$name" && break 2
    done
done

if [[ -z "$HELPER" ]]; then
    warn "Helper interface not found after USB rebind"
    warn "Cooperative RX will run with primary only"
else
    info "Helper interface: $HELPER"
fi

# --- Phase 6: Restart NetworkManager BEFORE pair/bind -------------------------
# NM must be restarted and interfaces set unmanaged BEFORE the helper enters
# monitor mode.  If NM starts after monitor mode is set, it may bring the
# helper interface down, killing the USB bulk-in URBs.  The driver's
# _netdev_open does not resubmit URBs on reopen, so recovery is impossible.

info "Restarting NetworkManager..."
systemctl start NetworkManager 2>/dev/null || true
sleep 2
nmcli device set "$PRIMARY" managed no 2>/dev/null || true
[[ -n "$HELPER" ]] && nmcli device set "$HELPER" managed no 2>/dev/null || true

# NM restart may flush routes — re-apply DHCP lease
if ! ip route show dev "$PRIMARY" 2>/dev/null | grep -q default; then
    info "Re-applying DHCP lease (NM flushed routes)..."
    dhcpcd -1 -4 "$PRIMARY" 2>/dev/null || true
fi

# --- Phase 7: Pair, bind, monitor mode ----------------------------------------

if [[ -n "$HELPER" ]]; then
    # Bring helper interface up — monitor mode is now set by the driver
    # internally when bind is called (rtw_coop_rx_enable_helper_monitor)
    ip link set "$HELPER" up

    info "Pairing helper..."
    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" \
        || fail "Pairing failed"

    info "Binding cooperative session (driver sets monitor mode on helpers)..."
    echo "1" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" \
        || fail "Bind failed"

    BOUND_CH=$(grep bound_channel /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null | awk '{print $2}')
    info "Helper in monitor mode on ch$BOUND_CH (set by driver)"
fi

# --- Verify -------------------------------------------------------------------

# Re-apply DHCP if NM flushed routes (check again after pair/bind)
if ! ip route show dev "$PRIMARY" 2>/dev/null | grep -q default; then
    info "Re-applying DHCP lease..."
    dhcpcd -1 -4 "$PRIMARY" 2>/dev/null || true
    sleep 1
fi

info "Verifying..."
GW=$(ip route show dev "$PRIMARY" 2>/dev/null | grep default | awk '{print $3}')
GW="${GW:-10.6.0.1}"
if ping -c 2 -W 2 -I "$PRIMARY" "$GW" &>/dev/null; then
    info "Ping to $GW OK"
else
    warn "Ping to $GW failed — run: dhcpcd -1 -4 $PRIMARY"
fi

echo ""
info "=== Cooperative RX Status ==="
cat /sys/kernel/debug/rtw_coop_rx/stats 2>/dev/null
echo ""
info "Commands:"
echo "  Monitor:  sudo python3 $(cd "$(dirname "$0")" && pwd)/coop-rx-monitor.py"
echo "  Test:     sudo $(cd "$(dirname "$0")" && pwd)/coop-rx-test.sh"
echo "  Stop:     sudo $(cd "$(dirname "$0")" && pwd)/coop-rx-stop.sh"
echo ""
echo "  For AP mode, see: coop-rx-ap-test-start.sh"
