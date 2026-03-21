#!/bin/bash
#
# coop-rx-ap-test-start.sh — Set up cooperative RX diversity in AP mode
#
# Tests the AP-mode cooperative RX path where the primary adapter runs
# as an AP (hostapd) and the helper captures STA→AP uplink frames
# (to_fr_ds=1) in monitor mode.
#
# Prerequisites:
#   - Two RTL8822CU USB adapters on this machine
#   - Remote device at REMOTE_HOST reachable via ethernet (SSH)
#   - Remote device has a WiFi interface that can connect as STA
#   - Module built and available at ../88x2cu.ko
#
# Test topology:
#   LOCAL (this machine)              REMOTE (via ethernet SSH)
#   ┌─────────────────────┐          ┌──────────────────────┐
#   │ 8812cu #1: AP mode  │◄─WiFi──►│ wlan0: STA client    │
#   │ (hostapd, primary)  │          │ (wpa_supplicant)     │
#   │                     │          │                      │
#   │ 8812cu #2: monitor  │          │ eth0: 192.168.1.13   │
#   │ (helper)            │          │ (SSH control path)   │
#   └─────────────────────┘          └──────────────────────┘
#
# Usage: sudo ./coop-rx-ap-test-start.sh [REMOTE_HOST]
#   REMOTE_HOST defaults to 192.168.1.13
#
# The script:
#   1. Stops remote hostapd (frees wlan0 for STA use)
#   2. Loads local driver, starts hostapd on primary adapter
#   3. Pairs helper, binds cooperative session (AP mode)
#   4. Connects remote as STA to local AP
#   5. Verifies connectivity and cooperative RX stats
#
# Cleanup: sudo ./coop-rx-ap-test-stop.sh [REMOTE_HOST]
#

set -euo pipefail

REMOTE_HOST="${1:-192.168.1.13}"
AP_SSID="coop-rx-ap-test"
AP_PSK="testpassword123"
AP_CHANNEL="${AP_CHANNEL:-149}"
AP_COUNTRY="${AP_COUNTRY:-SE}"
MODULE_PATH="$(cd "$(dirname "$0")/.." && pwd)/88x2cu.ko"

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
BOLD='\033[1m'
info()  { echo -e "${GRN}[+]${NC} $*"; }
warn()  { echo -e "${YLW}[!]${NC} $*"; }
fail()  { echo -e "${RED}[-]${NC} $*"; exit 1; }

[[ $EUID -eq 0 ]] || fail "Must run as root"

# --- Verify remote is reachable via ethernet ----------------------------------

info "Checking remote device ($REMOTE_HOST)..."
ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" "true" 2>/dev/null \
    || fail "Cannot SSH to root@${REMOTE_HOST} — is ethernet connected?"

REMOTE_WIFI=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "ls /sys/class/net/ | grep wlan | head -1" 2>/dev/null)
[[ -n "$REMOTE_WIFI" ]] || fail "No WiFi interface found on remote"
info "Remote WiFi interface: $REMOTE_WIFI"

# --- Find local RTL8822CU USB devices ----------------------------------------

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
    fail "Need 2 RTL8822CU USB devices, found ${#USB_DEVICES[@]}"
fi
info "USB devices: ${USB_DEVICES[*]}"

# --- Phase 1: Stop remote hostapd --------------------------------------------

info "Stopping remote hostapd..."
ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "killall hostapd 2>/dev/null; sleep 1; ip link set $REMOTE_WIFI down 2>/dev/null" \
    || warn "hostapd stop failed (may not be running)"
sleep 1

# --- Phase 2: Clean local state ----------------------------------------------

info "Stopping local NetworkManager and wpa_supplicant..."
systemctl stop NetworkManager 2>/dev/null || true
pkill wpa_supplicant 2>/dev/null || true
sleep 1

if lsmod | grep -q "^88x2cu"; then
    info "Unloading existing driver..."
    for iface in /sys/class/net/wl*; do
        [ -d "$iface" ] && ip link set "$(basename "$iface")" down 2>/dev/null
    done
    sleep 1
    rmmod 88x2cu 2>/dev/null || warn "rmmod failed"
    sleep 2
fi

# --- Phase 3: Load driver, identify interfaces -------------------------------

info "Loading driver..."
insmod "$MODULE_PATH" rtw_cooperative_rx=1 || fail "Failed to load module"
sleep 3

# Find both interfaces
IFACES=()
for dev in /sys/class/net/wl*; do
    [ -d "$dev/coop_rx" ] && IFACES+=("$(basename "$dev")")
done
[[ ${#IFACES[@]} -ge 2 ]] || fail "Need 2 RTW interfaces, found ${#IFACES[@]}"

PRIMARY="${IFACES[0]}"
HELPER="${IFACES[1]}"
info "Primary (AP): $PRIMARY"
info "Helper (monitor): $HELPER"

# --- Phase 4: Start hostapd on primary ----------------------------------------

HOSTAPD_CONF="/tmp/coop_rx_ap_test.conf"
cat > "$HOSTAPD_CONF" <<EOF
interface=$PRIMARY
driver=nl80211
ssid=$AP_SSID
country_code=$AP_COUNTRY
hw_mode=a
channel=$AP_CHANNEL
ieee80211n=1
ieee80211ac=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]
wpa=2
wpa_passphrase=$AP_PSK
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wmm_enabled=1
EOF

info "Setting regulatory domain to $AP_COUNTRY..."
iw reg set "$AP_COUNTRY" 2>/dev/null || true
sleep 1

info "Starting hostapd on $PRIMARY (SSID=$AP_SSID, ch=$AP_CHANNEL)..."
ip link set "$PRIMARY" up
hostapd -B "$HOSTAPD_CONF" || fail "hostapd failed to start"
sleep 2

# Assign IP to AP interface
ip addr add 10.7.0.1/24 dev "$PRIMARY" 2>/dev/null || true
info "AP interface IP: 10.7.0.1/24"

# --- Phase 5: Pair helper, bind cooperative session (AP mode) -----------------

info "Pairing helper..."
echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" \
    || fail "Pairing failed"

info "Binding cooperative session (AP mode)..."
echo 1 > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" \
    || fail "Bind failed — is AP in WIFI_AP_STATE?"

sleep 1
STATE=$(cat "/sys/class/net/$PRIMARY/coop_rx/coop_rx_info" | grep "state_name" | cut -d= -f2)
info "Cooperative state: $STATE"

if [[ "$STATE" != "ACTIVE" ]]; then
    warn "State is $STATE, expected ACTIVE"
fi

# --- Phase 6: Connect remote STA to our AP -----------------------------------

info "Connecting remote STA to local AP..."
ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" sh -s <<REMOTE_EOF
WIFI="$REMOTE_WIFI"

cat > /tmp/coop_ap_test_sta.conf <<WPA_EOF
ctrl_interface=/var/run/wpa_supplicant
network={
    ssid="$AP_SSID"
    psk="$AP_PSK"
    key_mgmt=WPA-PSK
    proto=RSN
    pairwise=CCMP
}
WPA_EOF

ip link set "\$WIFI" up
killall wpa_supplicant 2>/dev/null || true
sleep 1
wpa_supplicant -B -D nl80211 -i "\$WIFI" -c /tmp/coop_ap_test_sta.conf

i=0
while [ "\$i" -lt 15 ]; do
    iw dev "\$WIFI" link 2>/dev/null | grep -q "Connected" && break
    sleep 1
    i=\$((i + 1))
done

if iw dev "\$WIFI" link 2>/dev/null | grep -q "Connected"; then
    echo "CONNECTED"
    ip addr add 10.7.0.2/24 dev "\$WIFI" 2>/dev/null || true
else
    echo "FAILED"
fi
REMOTE_EOF

REMOTE_STATUS=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "iw dev $REMOTE_WIFI link 2>/dev/null | head -1")

if echo "$REMOTE_STATUS" | grep -q "Connected"; then
    info "Remote STA connected to our AP"
else
    warn "Remote STA connection status: $REMOTE_STATUS"
fi

# --- Phase 7: Verify ---------------------------------------------------------

sleep 2
info "Verifying connectivity..."

# Ping from AP to STA
if ping -c 3 -W 2 -I "$PRIMARY" 10.7.0.2 &>/dev/null; then
    info "Ping AP→STA: OK"
else
    warn "Ping AP→STA: FAILED (may need IP config)"
fi

# Ping from STA to AP
REMOTE_PING=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "ping -c 3 -W 2 -I $REMOTE_WIFI 10.7.0.1 2>&1 | tail -1" 2>/dev/null)
info "Remote ping result: $REMOTE_PING"

echo ""
info "=== Cooperative RX AP Mode Status ==="
cat "/sys/class/net/$PRIMARY/coop_rx/coop_rx_info"
echo ""
info "=== Stats ==="
cat "/sys/class/net/$PRIMARY/coop_rx/coop_rx_stats" | head -20

echo ""
info "Test with: sudo ./coop-rx-ap-test-verify.sh $REMOTE_HOST"
info "Stop with: sudo ./coop-rx-ap-test-stop.sh $REMOTE_HOST"
