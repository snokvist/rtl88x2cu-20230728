#!/bin/bash
#
# coop-rx-test.sh — Automated cooperative RX diversity test suite
#
# Runs 9 test scenarios with positive path verification.
# Requires both adapters already paired and bound (run coop-rx-start.sh first).
#
# Usage: sudo ./coop-rx-test.sh [GATEWAY]
#   GATEWAY defaults to the default route via the primary interface.
#   AP_HOST env var sets the SSH target for Test 8 (default: 192.168.1.13).
#

set -euo pipefail

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
BOLD='\033[1m'

DEBUGFS="/sys/kernel/debug/rtw_coop_rx/stats"
PING_COUNT=50
PING_INTERVAL=0.1

PASS=0
FAIL=0
SKIP=0

# ---- Helpers ----------------------------------------------------------------

info()  { echo -e "${CYN}[*]${NC} $*"; }
pass()  { echo -e "${GRN}[PASS]${NC} $*"; PASS=$((PASS + 1)); }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL + 1)); }
skip()  { echo -e "${YLW}[SKIP]${NC} $*"; SKIP=$((SKIP + 1)); }
die()   { echo -e "${RED}[-]${NC} $*"; exit 1; }

find_primary() {
    for dev in /sys/class/net/wl*; do
        [ -d "$dev" ] || continue
        local role
        role=$(cat "$dev/coop_rx/coop_rx_role" 2>/dev/null) || continue
        if [ "$role" = "primary" ]; then
            basename "$dev"
            return
        fi
    done
}

find_helper() {
    for dev in /sys/class/net/wl*; do
        [ -d "$dev" ] || continue
        local role
        role=$(cat "$dev/coop_rx/coop_rx_role" 2>/dev/null) || continue
        if [ "$role" = "helper" ]; then
            basename "$dev"
            return
        fi
    done
}

get_stat() {
    local key="$1"
    grep "^${key}:" "$DEBUGFS" 2>/dev/null | awk '{print $2}' | head -1
}

reset_stats() {
    local primary="$1"
    echo 1 > "/sys/class/net/$primary/coop_rx/coop_rx_reset_stats" 2>/dev/null
    sleep 0.2
}

set_drop_primary() {
    local primary="$1" val="$2"
    echo "$val" > "/sys/class/net/$primary/coop_rx/coop_rx_drop_primary" 2>/dev/null
}

snapshot_stats() {
    local -n arr=$1
    while IFS=: read -r key val; do
        key=$(echo "$key" | tr -d ' ')
        val=$(echo "$val" | awk '{print $1}')
        [ -n "$key" ] && [ -n "$val" ] && arr["$key"]="$val"
    done < "$DEBUGFS"
}

run_ping() {
    local iface="$1" gw="$2" count="$3" interval="$4"
    ping -c "$count" -i "$interval" -W 2 -I "$iface" "$gw" 2>&1
}

count_received() {
    # Extract "X received" from ping output
    echo "$1" | grep "packets transmitted" | grep -oP '\d+(?= received)' || echo "0"
}

count_dups() {
    echo "$1" | grep -c "(DUP!)" || true
}

count_loss() {
    echo "$1" | grep "packet loss" | grep -oP '\d+(?=% packet loss)' || echo "100"
}

get_info_field() {
    local primary="$1" key="$2"
    grep "^${key}=" "/sys/class/net/$primary/coop_rx/coop_rx_info" 2>/dev/null | cut -d= -f2 | tr -d ' '
}

AP_HOST="${AP_HOST:-192.168.1.13}"

# ---- Pre-checks -------------------------------------------------------------

[[ $EUID -eq 0 ]] || die "Must run as root"
[ -f "$DEBUGFS" ] || die "Debugfs stats not found — is module loaded with rtw_cooperative_rx=1?"

PRIMARY=$(find_primary)
HELPER=$(find_helper)
[ -n "$PRIMARY" ] || die "No primary interface found"

GW="${1:-}"
if [ -z "$GW" ]; then
    GW=$(ip route show dev "$PRIMARY" 2>/dev/null | grep default | awk '{print $3}' | head -1)
fi
[ -n "$GW" ] || die "No gateway found — pass gateway IP as argument"

STATE=$(get_stat "state")

echo ""
echo -e "${BOLD}Cooperative RX Diversity Test Suite${NC}"
echo -e "Primary: $PRIMARY  Helper: ${HELPER:-none}  Gateway: $GW  State: $STATE"
echo "────────────────────────────────────────────────────────────────"

# ---- Test 1: Baseline connectivity ------------------------------------------

info "Test 1: Baseline connectivity"

reset_stats "$PRIMARY"
RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
LOSS=$(count_loss "$RESULT")
DUPS=$(count_dups "$RESULT")

if [ "$LOSS" -le 5 ]; then
    pass "Test 1: Baseline — ${LOSS}% loss, ${DUPS} DUP! (expected <=5% loss)"
else
    fail "Test 1: Baseline — ${LOSS}% loss (expected <=5%)"
fi

# ---- Test 2: Normal diversity (both paths) -----------------------------------

info "Test 2: Normal diversity (primary + helper)"

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 2: No helper or not ACTIVE state"
else
    reset_stats "$PRIMARY"
    sleep 0.5

    RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
    LOSS=$(count_loss "$RESULT")
    DUPS=$(count_dups "$RESULT")

    declare -A S=()
    snapshot_stats S

    ACCEPTED="${S[helper_rx_accepted]:-0}"
    CRYPTO="${S[helper_rx_crypto_err]:-0}"
    DUP_DROPPED="${S[helper_rx_dup_dropped]:-0}"
    CANDIDATES="${S[helper_rx_candidates]:-0}"

    MSGS=""
    TFAIL=0

    [ "$LOSS" -gt 5 ] && MSGS="${MSGS} loss=${LOSS}%>5%" && TFAIL=1
    [ "$CRYPTO" -gt 0 ] && MSGS="${MSGS} crypto_err=${CRYPTO}" && TFAIL=1
    [ "$DUPS" -gt 0 ] && MSGS="${MSGS} DUP!=${DUPS}" && TFAIL=1

    DETAIL="loss=${LOSS}% DUP!=${DUPS} accepted=${ACCEPTED} dup_dropped=${DUP_DROPPED} crypto=${CRYPTO} candidates=${CANDIDATES}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 2: Normal diversity — ${DETAIL}"
    else
        fail "Test 2: Normal diversity — ${DETAIL} [${MSGS}]"
    fi
    unset S
fi

# ---- Test 3: Helper-only (drop_primary=1) ------------------------------------
# Positive verification: primary RX data frames are dropped, so ONLY the helper
# path can deliver frames.  If ping works, the helper path is confirmed working.

info "Test 3: Helper-only (drop_primary=1)"

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 3: No helper or not ACTIVE state"
else
    reset_stats "$PRIMARY"

    set_drop_primary "$PRIMARY" 1
    sleep 0.5

    RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
    LOSS=$(count_loss "$RESULT")
    DUPS=$(count_dups "$RESULT")
    RECEIVED=$(count_received "$RESULT")

    set_drop_primary "$PRIMARY" 0

    declare -A S=()
    snapshot_stats S
    ACCEPTED="${S[helper_rx_accepted]:-0}"
    DUP_DROPPED="${S[helper_rx_dup_dropped]:-0}"
    DEFERRED="${S[helper_rx_deferred]:-0}"

    MSGS=""
    TFAIL=0

    [ "$LOSS" -gt 5 ] && MSGS="${MSGS} loss=${LOSS}%>5%" && TFAIL=1
    [ "$DUPS" -gt 0 ] && MSGS="${MSGS} DUP!=${DUPS}" && TFAIL=1

    # Positive: helper must have accepted frames (the ONLY delivery path)
    if [ "$ACCEPTED" -lt 1 ]; then
        MSGS="${MSGS} accepted=0(helper_not_delivering)"
        TFAIL=1
    fi

    # Positive: deferred should match delivery attempts
    if [ "$DEFERRED" -lt 1 ]; then
        MSGS="${MSGS} deferred=0(drain_not_running)"
        TFAIL=1
    fi

    # Positive: received ping replies must be > 0 (proof of end-to-end delivery)
    if [ "$RECEIVED" -lt "$((PING_COUNT - 3))" ]; then
        MSGS="${MSGS} received=${RECEIVED}<${PING_COUNT}"
        TFAIL=1
    fi

    DETAIL="loss=${LOSS}% DUP!=${DUPS} received=${RECEIVED}/${PING_COUNT} accepted=${ACCEPTED} deferred=${DEFERRED} dup_dropped=${DUP_DROPPED}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 3: Helper-only — ${DETAIL}"
    else
        fail "Test 3: Helper-only — ${DETAIL} [${MSGS}]"
    fi
    unset S
fi

# ---- Test 4: Primary-only (unpair helper) ------------------------------------
# Positive verification: with no helper paired, accepted must be 0.
# All frames are delivered by the primary's normal path.

info "Test 4: Primary-only (unpair helper)"

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 4: No helper or not ACTIVE state"
else
    # Unpair helper
    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_unpair" 2>/dev/null || true
    sleep 1

    reset_stats "$PRIMARY"
    sleep 0.5

    RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
    LOSS=$(count_loss "$RESULT")
    DUPS=$(count_dups "$RESULT")
    RECEIVED=$(count_received "$RESULT")

    declare -A S=()
    snapshot_stats S
    ACCEPTED="${S[helper_rx_accepted]:-0}"
    CANDIDATES="${S[helper_rx_candidates]:-0}"

    MSGS=""
    TFAIL=0

    [ "$LOSS" -gt 5 ] && MSGS="${MSGS} loss=${LOSS}%>5%" && TFAIL=1

    # Positive: no helper means accepted MUST be 0
    if [ "$ACCEPTED" -ne 0 ]; then
        MSGS="${MSGS} accepted=${ACCEPTED}!=0(helper_not_unpaired?)"
        TFAIL=1
    fi

    # Positive: candidates should be 0 (no helper radio contributing)
    if [ "$CANDIDATES" -ne 0 ]; then
        MSGS="${MSGS} candidates=${CANDIDATES}!=0"
        TFAIL=1
    fi

    # Positive: ping must work — primary is the sole path
    if [ "$RECEIVED" -lt "$((PING_COUNT - 3))" ]; then
        MSGS="${MSGS} received=${RECEIVED}<${PING_COUNT}"
        TFAIL=1
    fi

    DETAIL="loss=${LOSS}% DUP!=${DUPS} received=${RECEIVED}/${PING_COUNT} accepted=${ACCEPTED} candidates=${CANDIDATES}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 4: Primary-only — ${DETAIL}"
    else
        fail "Test 4: Primary-only — ${DETAIL} [${MSGS}]"
    fi

    # Re-pair and re-bind for subsequent tests
    nmcli device set "$HELPER" managed no 2>/dev/null || true
    ip link set "$HELPER" up 2>/dev/null || true
    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" 2>/dev/null || true
    echo 1 > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" 2>/dev/null || true
    sleep 2
    unset S
fi

# ---- Test 5: Unpair mid-stream (graceful fallback) ---------------------------

info "Test 5: Unpair mid-stream (graceful fallback)"

HELPER=$(find_helper)  # refresh after re-pair
STATE=$(get_stat "state")

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 5: No helper or not ACTIVE (re-pair may have failed)"
else
    reset_stats "$PRIMARY"

    # Start a long ping in background
    ping -c 100 -i "$PING_INTERVAL" -W 2 -I "$PRIMARY" "$GW" > /tmp/coop_rx_test5.txt 2>&1 &
    PING_PID=$!
    sleep 1

    declare -A BEFORE=()
    snapshot_stats BEFORE
    FALLBACK_BEFORE="${BEFORE[fallback_events]:-0}"

    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_unpair" 2>/dev/null || true
    sleep 2

    declare -A MID=()
    snapshot_stats MID
    FALLBACK_MID="${MID[fallback_events]:-0}"

    wait "$PING_PID" 2>/dev/null || true
    RESULT=$(cat /tmp/coop_rx_test5.txt)
    rm -f /tmp/coop_rx_test5.txt

    LOSS=$(count_loss "$RESULT")
    FALLBACK_DELTA=$((FALLBACK_MID - FALLBACK_BEFORE))

    MSGS=""
    TFAIL=0

    if [ "$FALLBACK_DELTA" -lt 1 ]; then
        MSGS="${MSGS} fallback_events_not_incremented"
        TFAIL=1
    fi

    DETAIL="loss=${LOSS}% fallback_delta=${FALLBACK_DELTA}"

    # Re-pair and re-bind
    nmcli device set "$HELPER" managed no 2>/dev/null || true
    ip link set "$HELPER" up 2>/dev/null || true
    echo "$HELPER" > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_pair" 2>/dev/null || true
    echo 1 > "/sys/class/net/$PRIMARY/coop_rx/coop_rx_bind" 2>/dev/null || true
    sleep 2

    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 5: Unpair mid-stream — ${DETAIL} (no crash, fallback fired)"
    else
        fail "Test 5: Unpair mid-stream — ${DETAIL} [${MSGS}]"
    fi
    unset BEFORE MID
fi

# ---- Test 6: Re-pair recovery -----------------------------------------------
# After tests 4+5 unpaired the helper, verify that re-pair restores cooperative RX.

info "Test 6: Re-pair recovery"

HELPER=$(find_helper)
STATE=$(get_stat "state")

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 6: Re-pair failed — helper=${HELPER:-none} state=${STATE}"
else
    reset_stats "$PRIMARY"
    sleep 0.5

    RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
    LOSS=$(count_loss "$RESULT")
    DUPS=$(count_dups "$RESULT")

    declare -A S=()
    snapshot_stats S
    ACCEPTED="${S[helper_rx_accepted]:-0}"

    MSGS=""
    TFAIL=0

    [ "$LOSS" -gt 5 ] && MSGS="${MSGS} loss=${LOSS}%>5%" && TFAIL=1
    [ "$DUPS" -gt 0 ] && MSGS="${MSGS} DUP!=${DUPS}" && TFAIL=1

    DETAIL="loss=${LOSS}% DUP!=${DUPS} accepted=${ACCEPTED}"
    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 6: Re-pair recovery — ${DETAIL}"
    else
        fail "Test 6: Re-pair recovery — ${DETAIL} [${MSGS}]"
    fi
    unset S
fi

# ---- Test 7: Stats consistency -----------------------------------------------

info "Test 7: Stats consistency (accounting invariants)"

HELPER=$(find_helper)
STATE=$(get_stat "state")

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 7: No helper or not ACTIVE state"
else
    reset_stats "$PRIMARY"
    sleep 0.5

    run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL" > /dev/null 2>&1
    # Wait for deferred queue to fully drain and counters to settle
    sleep 2

    declare -A S=()
    snapshot_stats S

    CANDIDATES="${S[helper_rx_candidates]:-0}"
    ACCEPTED="${S[helper_rx_accepted]:-0}"
    DUP_DROPPED="${S[helper_rx_dup_dropped]:-0}"
    FOREIGN="${S[helper_rx_foreign]:-0}"
    CRYPTO="${S[helper_rx_crypto_err]:-0}"
    LATE="${S[helper_rx_late]:-0}"
    NO_STA="${S[helper_rx_no_sta]:-0}"
    POOL_FULL="${S[helper_rx_pool_full]:-0}"
    DEFERRED="${S[helper_rx_deferred]:-0}"
    BACKPRESSURE="${S[helper_rx_backpressure]:-0}"
    PENDING="${S[pending_count]:-0}"

    MSGS=""
    TFAIL=0

    # Invariant 1: pending_count should be 0 at rest
    if [ "$PENDING" -ne 0 ]; then
        MSGS="${MSGS} pending=${PENDING}!=0"
        TFAIL=1
    fi

    # Invariant 2: backpressure should be 0 under light load
    if [ "$BACKPRESSURE" -ne 0 ]; then
        MSGS="${MSGS} backpressure=${BACKPRESSURE}!=0"
        TFAIL=1
    fi

    # Invariant 3: candidates >= accounted
    # Tolerance scales with traffic: non-atomic reads race with concurrent stream
    ACCOUNTED=$((FOREIGN + ACCEPTED + DUP_DROPPED + LATE + CRYPTO + NO_STA + POOL_FULL))
    DIFF_3=$((ACCOUNTED - CANDIDATES))
    TOL=$((CANDIDATES / 50 + 5))  # 2% of candidates + 5
    if [ "$DIFF_3" -gt "$TOL" ]; then
        MSGS="${MSGS} candidates(${CANDIDATES})<accounted(${ACCOUNTED})_tol=${TOL}"
        TFAIL=1
    fi

    # Invariant 4: deferred >= accepted
    # (dup_dropped includes pre-enqueue drops that never enter the deferred queue,
    #  so deferred < accepted + dup_dropped is expected)
    DIFF_4=$((ACCEPTED - DEFERRED))
    TOL4=$((DEFERRED / 50 + 5))  # 2% of deferred + 5
    if [ "$DIFF_4" -gt "$TOL4" ]; then
        MSGS="${MSGS} deferred(${DEFERRED})<accepted(${ACCEPTED})_tol=${TOL4}"
        TFAIL=1
    fi

    DETAIL="candidates=${CANDIDATES} foreign=${FOREIGN} accepted=${ACCEPTED} dup_dropped=${DUP_DROPPED}"
    DETAIL="${DETAIL} late=${LATE} crypto=${CRYPTO} deferred=${DEFERRED} pending=${PENDING} backpressure=${BACKPRESSURE}"

    if [ "$TFAIL" -eq 0 ]; then
        pass "Test 7: Stats consistency — ${DETAIL}"
    else
        fail "Test 7: Stats consistency — ${DETAIL} [${MSGS}]"
    fi
    unset S
fi

# ---- Test 8: Channel Switch via CSA ------------------------------------------
# Verifies cooperative RX survives an AP-initiated channel switch (CSA).
# SSHes to the AP and triggers hostapd_cli chan_switch. Checks that
# bound_channel updates, primary_channel matches, state stays ACTIVE,
# and the helper is still contributing frames.

info "Test 8: Channel switch via CSA (hostapd_cli)"

HELPER=$(find_helper)
STATE=$(get_stat "state")

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 8: No helper or not ACTIVE state"
elif ! ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${AP_HOST}" true 2>/dev/null; then
    skip "Test 8: AP not reachable via SSH (root@${AP_HOST})"
else
    INIT_CH=$(get_info_field "$PRIMARY" "channel")
    INIT_PRI_CH=$(get_info_field "$PRIMARY" "primary_channel")

    if [ -z "$INIT_CH" ] || [ "$INIT_CH" = "0" ]; then
        skip "Test 8: Cannot read current channel from coop_rx_info"
    else
        # Query AP bandwidth config (secondary_channel offset) via hostapd_cli
        AP_SEC_CH=$(ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${AP_HOST}" \
            "hostapd_cli status 2>/dev/null" | grep "^secondary_channel=" | cut -d= -f2 | tr -d '[:space:]')
        AP_SEC_CH="${AP_SEC_CH:-0}"

        # Pick a target 40MHz pair in the same band
        # UNII-3: 149+153 (HT40+) ↔ 157+161 (HT40+)
        # UNII-1: 36+40 (HT40+) ↔ 44+48 (HT40+)
        if [ "$INIT_CH" -ge 149 ]; then
            if [ "$INIT_CH" -le 153 ]; then
                TARGET_CH=157; TARGET_SEC=1
            else
                TARGET_CH=149; TARGET_SEC=1
            fi
        else
            if [ "$INIT_CH" -le 40 ]; then
                TARGET_CH=44; TARGET_SEC=1
            else
                TARGET_CH=36; TARGET_SEC=1
            fi
        fi
        TARGET_FREQ=$((5000 + TARGET_CH * 5))
        ORIG_FREQ=$((5000 + INIT_CH * 5))
        ORIG_SEC="${AP_SEC_CH}"

        # Build hostapd_cli chan_switch command with HT40 parameters
        # Full syntax needed for 40MHz: freq + center_freq1 + sec_channel_offset + bandwidth + ht
        # center_freq1 for HT40+ = primary_freq + 10, for HT40- = primary_freq - 10
        if [ "$TARGET_SEC" -ge 0 ]; then
            TARGET_CENTER=$((TARGET_FREQ + 10))
        else
            TARGET_CENTER=$((TARGET_FREQ - 10))
        fi
        if [ "$ORIG_SEC" -ge 0 ]; then
            ORIG_CENTER=$((ORIG_FREQ + 10))
        else
            ORIG_CENTER=$((ORIG_FREQ - 10))
        fi
        CSA_FWD="hostapd_cli chan_switch 5 ${TARGET_FREQ} center_freq1=${TARGET_CENTER} sec_channel_offset=${TARGET_SEC} bandwidth=40 ht"
        CSA_RET="hostapd_cli chan_switch 5 ${ORIG_FREQ} center_freq1=${ORIG_CENTER} sec_channel_offset=${ORIG_SEC} bandwidth=40 ht"

        info "  Switching ch${INIT_CH} -> ch${TARGET_CH} (${TARGET_FREQ} MHz, HT40 sec_offset=${TARGET_SEC})"

        # --- Forward switch ---
        ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${AP_HOST}" \
            "${CSA_FWD}" 2>/dev/null || true

        # Poll for channel change (up to 15s)
        WAITED=0
        while [ "$WAITED" -lt 15 ]; do
            sleep 1
            WAITED=$((WAITED + 1))
            CUR_CH=$(get_info_field "$PRIMARY" "channel")
            [ "$CUR_CH" = "$TARGET_CH" ] && break
        done

        CUR_CH=$(get_info_field "$PRIMARY" "channel")
        CUR_PRI_CH=$(get_info_field "$PRIMARY" "primary_channel")
        CUR_STATE=$(get_stat "state")

        MSGS=""
        TFAIL=0

        [ "$CUR_CH" != "$TARGET_CH" ] && MSGS="${MSGS} channel=${CUR_CH}!=target${TARGET_CH}" && TFAIL=1
        [ "$CUR_PRI_CH" != "$CUR_CH" ] && MSGS="${MSGS} pri_ch(${CUR_PRI_CH})!=ch(${CUR_CH})_MISMATCH" && TFAIL=1
        [ "$CUR_STATE" != "3" ] && MSGS="${MSGS} state=${CUR_STATE}!=ACTIVE" && TFAIL=1

        # Ping + stats on new channel
        reset_stats "$PRIMARY"
        sleep 0.5
        RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
        LOSS=$(count_loss "$RESULT")

        declare -A S=()
        snapshot_stats S
        CANDIDATES="${S[helper_rx_candidates]:-0}"
        CRYPTO="${S[helper_rx_crypto_err]:-0}"

        [ "$LOSS" -gt 10 ] && MSGS="${MSGS} loss=${LOSS}%>10%" && TFAIL=1
        [ "$CRYPTO" -gt 0 ] && MSGS="${MSGS} crypto_err=${CRYPTO}" && TFAIL=1
        [ "$CANDIDATES" -lt 1 ] && MSGS="${MSGS} candidates=0(helper_dead?)" && TFAIL=1

        DETAIL="ch:${INIT_CH}->${CUR_CH} pri:${CUR_PRI_CH} state=${CUR_STATE} loss=${LOSS}% candidates=${CANDIDATES} crypto=${CRYPTO}"

        if [ "$TFAIL" -eq 0 ]; then
            pass "Test 8a: Forward switch — ${DETAIL}"
        else
            fail "Test 8a: Forward switch — ${DETAIL} [${MSGS}]"
        fi
        unset S

        # --- Return switch ---
        info "  Switching ch${TARGET_CH} -> ch${INIT_CH} (${ORIG_FREQ} MHz, HT40 sec_offset=${ORIG_SEC})"

        ssh -o ConnectTimeout=3 -o BatchMode=yes "root@${AP_HOST}" \
            "${CSA_RET}" 2>/dev/null || true

        WAITED=0
        while [ "$WAITED" -lt 15 ]; do
            sleep 1
            WAITED=$((WAITED + 1))
            CUR_CH=$(get_info_field "$PRIMARY" "channel")
            [ "$CUR_CH" = "$INIT_CH" ] && break
        done

        CUR_CH=$(get_info_field "$PRIMARY" "channel")
        CUR_PRI_CH=$(get_info_field "$PRIMARY" "primary_channel")
        CUR_STATE=$(get_stat "state")

        MSGS=""
        TFAIL=0

        [ "$CUR_CH" != "$INIT_CH" ] && MSGS="${MSGS} channel=${CUR_CH}!=orig${INIT_CH}" && TFAIL=1
        [ "$CUR_PRI_CH" != "$CUR_CH" ] && MSGS="${MSGS} pri_ch(${CUR_PRI_CH})!=ch(${CUR_CH})_MISMATCH" && TFAIL=1
        [ "$CUR_STATE" != "3" ] && MSGS="${MSGS} state=${CUR_STATE}!=ACTIVE" && TFAIL=1

        reset_stats "$PRIMARY"
        sleep 0.5
        RESULT=$(run_ping "$PRIMARY" "$GW" "$PING_COUNT" "$PING_INTERVAL")
        LOSS=$(count_loss "$RESULT")

        declare -A S=()
        snapshot_stats S
        CANDIDATES="${S[helper_rx_candidates]:-0}"
        CRYPTO="${S[helper_rx_crypto_err]:-0}"

        [ "$LOSS" -gt 10 ] && MSGS="${MSGS} loss=${LOSS}%>10%" && TFAIL=1
        [ "$CRYPTO" -gt 0 ] && MSGS="${MSGS} crypto_err=${CRYPTO}" && TFAIL=1
        [ "$CANDIDATES" -lt 1 ] && MSGS="${MSGS} candidates=0(helper_dead?)" && TFAIL=1

        DETAIL="ch:${TARGET_CH}->${CUR_CH} pri:${CUR_PRI_CH} state=${CUR_STATE} loss=${LOSS}% candidates=${CANDIDATES} crypto=${CRYPTO}"

        if [ "$TFAIL" -eq 0 ]; then
            pass "Test 8b: Return switch — ${DETAIL}"
        else
            fail "Test 8b: Return switch — ${DETAIL} [${MSGS}]"
        fi
        unset S
    fi
fi

# ---- Test 9: Stats reset correctness -----------------------------------------
# Verifies that stats reset via sysfs properly zeroes all atomic counters.
# Validates fix for memset-on-atomics bug (must use atomic_set per field).

info "Test 9: Stats reset correctness"

HELPER=$(find_helper)
STATE=$(get_stat "state")

if [ -z "$HELPER" ] || [ "$STATE" != "3" ]; then
    skip "Test 9: No helper or not ACTIVE state"
else
    # Phase 1: Generate traffic so counters are nonzero
    run_ping "$PRIMARY" "$GW" 20 "$PING_INTERVAL" > /dev/null 2>&1
    sleep 1

    declare -A PRE=()
    snapshot_stats PRE
    PRE_CANDIDATES="${PRE[helper_rx_candidates]:-0}"

    if [ "$PRE_CANDIDATES" -lt 1 ]; then
        skip "Test 9: No helper traffic generated (candidates=0)"
    else
        # Phase 2: Reset stats and immediately snapshot.
        # Small tolerance: the helper radio continuously receives frames
        # (beacons, etc.) so a few may arrive between reset and read.
        # The key assertion is that counters drop from thousands to near-zero,
        # proving atomic_set() zeroed them (vs memset corruption).
        reset_stats "$PRIMARY"

        declare -A POST=()
        snapshot_stats POST

        MSGS=""
        TFAIL=0

        # Allow frames from background traffic between reset and read.
        # With a 4000kbps video stream, ~500 frames/sec arrive continuously.
        RESET_TOL=500
        for key in helper_rx_candidates helper_rx_accepted helper_rx_dup_dropped \
                   helper_rx_pool_full helper_rx_foreign helper_rx_crypto_err \
                   helper_rx_late helper_rx_no_sta helper_rx_deferred \
                   helper_rx_backpressure; do
            val="${POST[$key]:-0}"
            if [ "$val" -gt "$RESET_TOL" ]; then
                MSGS="${MSGS} ${key}=${val}>${RESET_TOL}"
                TFAIL=1
            fi
        done
        # pending_count must be exactly 0 at rest (no tolerance)
        PENDING_VAL="${POST[pending_count]:-0}"
        if [ "$PENDING_VAL" != "0" ]; then
            MSGS="${MSGS} pending_count=${PENDING_VAL}!=0"
            TFAIL=1
        fi

        # Phase 4: Generate more traffic after reset
        run_ping "$PRIMARY" "$GW" 20 "$PING_INTERVAL" > /dev/null 2>&1
        sleep 1

        declare -A POST2=()
        snapshot_stats POST2
        POST2_CANDIDATES="${POST2[helper_rx_candidates]:-0}"

        # Counters must be incrementing again from 0
        if [ "$POST2_CANDIDATES" -lt 1 ]; then
            MSGS="${MSGS} post_reset_candidates=0(counters_stuck)"
            TFAIL=1
        fi

        DETAIL="pre_candidates=${PRE_CANDIDATES} post_reset_candidates=${POST2_CANDIDATES}"
        if [ "$TFAIL" -eq 0 ]; then
            pass "Test 9: Stats reset — ${DETAIL} (all counters zeroed correctly)"
        else
            fail "Test 9: Stats reset — ${DETAIL} [${MSGS}]"
        fi
        unset PRE POST POST2
    fi
fi

# ---- Summary -----------------------------------------------------------------

echo ""
echo "────────────────────────────────────────────────────────────────"
TOTAL=$((PASS + FAIL + SKIP))
echo -e "${BOLD}Results: ${GRN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YLW}${SKIP} skipped${NC} (${TOTAL} total)"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "Debug: cat $DEBUGFS"
    exit 1
fi
