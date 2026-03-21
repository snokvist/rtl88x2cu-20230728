#!/bin/bash
#
# coop-rx-ap-test-verify.sh — Verify cooperative RX in AP mode
#
# Runs validation tests with the primary as AP and a remote STA client.
# Must be run after coop-rx-ap-test-start.sh sets up the topology.
#
# Usage: sudo ./coop-rx-ap-test-verify.sh [REMOTE_HOST]
#

set -euo pipefail

REMOTE_HOST="${1:-192.168.1.13}"

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
BOLD='\033[1m'
info()  { echo -e "${CYN}[*]${NC} $*"; }
pass()  { echo -e "${GRN}[PASS]${NC} $*"; PASS=$((PASS + 1)); }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL + 1)); }
skip()  { echo -e "${YLW}[SKIP]${NC} $*"; SKIP=$((SKIP + 1)); }

PASS=0; FAIL=0; SKIP=0
DEBUGFS="/sys/kernel/debug/rtw_coop_rx/stats"

find_primary() {
    for dev in /sys/class/net/wl*; do
        [ -d "$dev" ] || continue
        local role=$(cat "$dev/coop_rx/coop_rx_role" 2>/dev/null) || continue
        [ "$role" = "primary" ] && basename "$dev" && return
    done
}

find_helper() {
    for dev in /sys/class/net/wl*; do
        [ -d "$dev" ] || continue
        local role=$(cat "$dev/coop_rx/coop_rx_role" 2>/dev/null) || continue
        [ "$role" = "helper" ] && basename "$dev" && return
    done
}

get_stat() {
    grep "^${1}:" "$DEBUGFS" 2>/dev/null | awk '{print $2}' | head -1
}

reset_stats() {
    echo 1 > "/sys/class/net/$1/coop_rx/coop_rx_reset_stats" 2>/dev/null
    sleep 0.2
}

get_info_field() {
    grep "^${2}=" "/sys/class/net/$1/coop_rx/coop_rx_info" 2>/dev/null | cut -d= -f2 | tr -d ' '
}

[[ $EUID -eq 0 ]] || { echo "Must run as root"; exit 1; }
[ -f "$DEBUGFS" ] || { echo "Debugfs not found — is driver loaded?"; exit 1; }

PRIMARY=$(find_primary)
HELPER=$(find_helper)
[ -n "$PRIMARY" ] || { echo "No primary found"; exit 1; }

STATE=$(get_stat "state")
REMOTE_WIFI=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "ls /sys/class/net/ | grep wlan | head -1" 2>/dev/null)

echo ""
echo -e "${BOLD}Cooperative RX AP Mode — Verification${NC}"
echo -e "Primary (AP): $PRIMARY  Helper: ${HELPER:-none}  State: $STATE"
echo -e "Remote STA: ${REMOTE_HOST} (${REMOTE_WIFI:-?})"
echo "────────────────────────────────────────────────────────────────"

# ---- Test 1: AP state and binding --------------------------------------------

info "Test 1: AP mode binding"

STATE_NAME=$(get_info_field "$PRIMARY" "state_name")
BSSID=$(get_info_field "$PRIMARY" "bssid")
PRI_MAC=$(cat /sys/class/net/$PRIMARY/address 2>/dev/null | tr '[:lower:]' '[:upper:]')
BSSID_UPPER=$(echo "$BSSID" | tr '[:lower:]' '[:upper:]' | tr ':' ':')

MSGS=""; TFAIL=0

[ "$STATE_NAME" != "ACTIVE" ] && MSGS="${MSGS} state=${STATE_NAME}!=ACTIVE" && TFAIL=1
[ -z "$HELPER" ] && MSGS="${MSGS} no_helper" && TFAIL=1

# In AP mode, bound_bssid should be our own MAC
if [ -n "$PRI_MAC" ] && [ -n "$BSSID" ]; then
    # Normalize for comparison
    BSSID_NORM=$(echo "$BSSID" | tr '[:lower:]' '[:upper:]' | tr -d ':')
    PRI_NORM=$(echo "$PRI_MAC" | tr '[:lower:]' '[:upper:]' | tr -d ':')
    [ "$BSSID_NORM" != "$PRI_NORM" ] && MSGS="${MSGS} bssid_mismatch(${BSSID}!=${PRI_MAC})" && TFAIL=1
fi

if [ "$TFAIL" -eq 0 ]; then
    pass "Test 1: AP mode binding — state=$STATE_NAME bssid=$BSSID (matches AP MAC)"
else
    fail "Test 1: AP mode binding — state=$STATE_NAME bssid=$BSSID [${MSGS}]"
fi

# ---- Test 2: Remote STA connected -------------------------------------------

info "Test 2: Remote STA association"

REMOTE_LINK=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
    "iw dev $REMOTE_WIFI link 2>/dev/null | head -3" 2>/dev/null)

if echo "$REMOTE_LINK" | grep -q "Connected"; then
    pass "Test 2: Remote STA connected to our AP"
else
    fail "Test 2: Remote STA not connected — $REMOTE_LINK"
fi

# ---- Test 3: Uplink traffic via cooperative RX (normal mode) -----------------

info "Test 3: Uplink cooperative RX (STA→AP)"

if [ "$STATE" != "3" ] || [ -z "$HELPER" ]; then
    skip "Test 3: Not ACTIVE or no helper"
else
    reset_stats "$PRIMARY"

    # Generate uplink traffic: remote pings us
    ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "ping -c 30 -i 0.1 -W 2 -I $REMOTE_WIFI 10.7.0.1" >/dev/null 2>&1

    sleep 1
    CANDIDATES=$(get_stat "helper_rx_candidates")
    ACCEPTED=$(get_stat "helper_rx_accepted")
    DUP=$(get_stat "helper_rx_dup_dropped")
    FOREIGN=$(get_stat "helper_rx_foreign")
    CRYPTO=$(get_stat "helper_rx_crypto_err")
    NO_STA=$(get_stat "helper_rx_no_sta")
    DEFERRED=$(get_stat "helper_rx_deferred")

    MSGS=""; TFAIL=0

    [ "$CANDIDATES" -lt 1 ] && MSGS="${MSGS} candidates=0(helper_not_receiving)" && TFAIL=1
    [ "$CRYPTO" -gt 0 ] && MSGS="${MSGS} crypto_err=${CRYPTO}" && TFAIL=1
    [ "$NO_STA" -gt 0 ] && MSGS="${MSGS} no_sta=${NO_STA}(STA_lookup_failed)" && TFAIL=1

    DETAIL="candidates=${CANDIDATES} accepted=${ACCEPTED} dup=${DUP} foreign=${FOREIGN} crypto=${CRYPTO} no_sta=${NO_STA} deferred=${DEFERRED}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 3: Uplink cooperative RX — ${DETAIL}"
    else
        fail "Test 3: Uplink cooperative RX — ${DETAIL} [${MSGS}]"
    fi
fi

# ---- Test 4: Helper-only mode (drop_primary=1) ------------------------------

info "Test 4: Helper-only AP mode (drop_primary=1)"

if [ "$STATE" != "3" ] || [ -z "$HELPER" ]; then
    skip "Test 4: Not ACTIVE or no helper"
else
    reset_stats "$PRIMARY"
    echo 1 > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_drop_primary" 2>/dev/null
    sleep 0.5

    # Generate uplink traffic
    PING_OUT=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "ping -c 30 -i 0.1 -W 2 -I $REMOTE_WIFI 10.7.0.1 2>&1 | tail -2" 2>/dev/null)

    echo 0 > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_drop_primary" 2>/dev/null
    sleep 1

    ACCEPTED=$(get_stat "helper_rx_accepted")
    DEFERRED=$(get_stat "helper_rx_deferred")
    RECEIVED=$(echo "$PING_OUT" | grep "packets transmitted" | grep -oP '\d+(?= received)' || echo "0")

    MSGS=""; TFAIL=0

    [ "$ACCEPTED" -lt 1 ] && MSGS="${MSGS} accepted=0(helper_not_delivering)" && TFAIL=1
    [ "$DEFERRED" -lt 1 ] && MSGS="${MSGS} deferred=0(drain_not_running)" && TFAIL=1

    DETAIL="received=${RECEIVED}/30 accepted=${ACCEPTED} deferred=${DEFERRED}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 4: Helper-only AP — ${DETAIL}"
    else
        fail "Test 4: Helper-only AP — ${DETAIL} [${MSGS}]"
    fi
fi

# ---- Test 5: Stats consistency -----------------------------------------------

info "Test 5: AP mode stats consistency"

if [ "$STATE" != "3" ] || [ -z "$HELPER" ]; then
    skip "Test 5: Not ACTIVE or no helper"
else
    reset_stats "$PRIMARY"

    ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "ping -c 30 -i 0.1 -W 2 -I $REMOTE_WIFI 10.7.0.1" >/dev/null 2>&1
    sleep 2

    CANDIDATES=$(get_stat "helper_rx_candidates")
    ACCEPTED=$(get_stat "helper_rx_accepted")
    DUP=$(get_stat "helper_rx_dup_dropped")
    FOREIGN=$(get_stat "helper_rx_foreign")
    CRYPTO=$(get_stat "helper_rx_crypto_err")
    LATE=$(get_stat "helper_rx_late")
    NO_STA=$(get_stat "helper_rx_no_sta")
    POOL=$(get_stat "helper_rx_pool_full")
    PENDING=$(get_stat "pending_count")
    BACKPRESSURE=$(get_stat "helper_rx_backpressure")

    MSGS=""; TFAIL=0

    [ "$PENDING" -ne 0 ] && MSGS="${MSGS} pending=${PENDING}!=0" && TFAIL=1
    [ "$BACKPRESSURE" -ne 0 ] && MSGS="${MSGS} backpressure=${BACKPRESSURE}" && TFAIL=1

    ACCOUNTED=$((FOREIGN + ACCEPTED + DUP + LATE + CRYPTO + NO_STA + POOL))
    TOL=$((CANDIDATES / 50 + 5))
    DIFF=$((ACCOUNTED - CANDIDATES))
    [ "$DIFF" -gt "$TOL" ] && MSGS="${MSGS} accounting_mismatch(${ACCOUNTED}>${CANDIDATES}+${TOL})" && TFAIL=1

    DETAIL="candidates=${CANDIDATES} accepted=${ACCEPTED} dup=${DUP} foreign=${FOREIGN} late=${LATE} pending=${PENDING}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 5: Stats consistency — ${DETAIL}"
    else
        fail "Test 5: Stats consistency — ${DETAIL} [${MSGS}]"
    fi
fi

# ---- Test 6: Bidirectional traffic -------------------------------------------

info "Test 6: Bidirectional traffic (AP↔STA)"

if [ "$STATE" != "3" ] || [ -z "$HELPER" ]; then
    skip "Test 6: Not ACTIVE or no helper"
else
    reset_stats "$PRIMARY"

    # Uplink: remote pings us (background)
    ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${REMOTE_HOST}" \
        "ping -c 20 -i 0.1 -W 2 -I $REMOTE_WIFI 10.7.0.1 &>/dev/null &" 2>/dev/null

    # Downlink: we ping remote
    ping -c 20 -i 0.1 -W 2 -I "$PRIMARY" 10.7.0.2 >/dev/null 2>&1 || true
    sleep 2

    CANDIDATES=$(get_stat "helper_rx_candidates")
    ACCEPTED=$(get_stat "helper_rx_accepted")
    FOREIGN=$(get_stat "helper_rx_foreign")

    MSGS=""; TFAIL=0

    # Helper should only capture uplink (to_fr_ds=1), not downlink
    [ "$CANDIDATES" -lt 1 ] && MSGS="${MSGS} candidates=0" && TFAIL=1

    DETAIL="candidates=${CANDIDATES} accepted=${ACCEPTED} foreign=${FOREIGN}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 6: Bidirectional — ${DETAIL}"
    else
        fail "Test 6: Bidirectional — ${DETAIL} [${MSGS}]"
    fi
fi

# ---- Summary ----------------------------------------------------------------

echo ""
echo "────────────────────────────────────────────────────────────────"
TOTAL=$((PASS + FAIL + SKIP))
echo -e "${BOLD}Results: ${GRN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YLW}${SKIP} skipped${NC} (${TOTAL} total)"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "Debug: cat /sys/class/net/$PRIMARY/coop_rx/coop_rx_info"
    echo "       cat $DEBUGFS"
    exit 1
fi
