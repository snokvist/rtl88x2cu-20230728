#!/usr/bin/env python3
"""
coop-rx-monitor — Interactive monitor for RTL8822CU cooperative RX diversity

Reads /sys/kernel/debug/rtw_coop_rx/stats and the driver's coop_rx_info
sysfs endpoint to display a live dashboard of cooperative RX performance.
Supports both STA and AP primary modes.

Usage:
  sudo python3 coop-rx-monitor.py           # curses TUI dashboard
  sudo python3 coop-rx-monitor.py --status  # one-shot stdout summary

Keys (TUI mode):
  q / ESC  — quit
  r        — reset cooperative RX stats
  d        — toggle drop_primary (helper-only test mode)
"""

import curses
import time
import os
import sys

STATS_PATH = "/sys/kernel/debug/rtw_coop_rx/stats"
REFRESH_HZ = 4  # updates per second

# Smoothing factor for exponential moving average (0..1, lower = smoother)
EMA_ALPHA = 0.3


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except (OSError, PermissionError):
        return None


def write_file(path, value):
    try:
        with open(path, "w") as f:
            f.write(value)
        return True
    except (OSError, PermissionError):
        return False


def parse_stats():
    raw = read_file(STATS_PATH)
    if not raw:
        return None
    stats = {}
    for line in raw.splitlines():
        line = line.strip()
        if ":" in line and not line.startswith("===") and not line.startswith("---"):
            key, _, val = line.partition(":")
            val = val.strip()
            if key.strip() == "bound_bssid":
                stats[key.strip()] = val
            else:
                num = val.split()[0] if val else val
                try:
                    stats[key.strip()] = int(num)
                except ValueError:
                    stats[key.strip()] = val
    return stats


def parse_info(primary):
    """Read coop_rx_info sysfs — key=value per line."""
    path = f"/sys/class/net/{primary}/coop_rx/coop_rx_info"
    raw = read_file(path)
    if not raw:
        return {}
    info = {}
    for line in raw.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            info[k.strip()] = v.strip()
    return info


def find_coop_ifaces():
    """Find primary and helper interfaces."""
    primary = helper = None
    for name in sorted(os.listdir("/sys/class/net")):
        role_path = f"/sys/class/net/{name}/coop_rx/coop_rx_role"
        role = read_file(role_path)
        if role == "primary":
            primary = name
        elif role == "helper":
            helper = name
    return primary, helper


def fmt_bytes(n):
    if n >= 1_000_000_000:
        return f"{n / 1_000_000_000:.1f} GB"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f} MB"
    if n >= 1_000:
        return f"{n / 1_000:.1f} KB"
    return f"{n} B"


def fmt_rate(bps):
    if abs(bps) >= 1_000_000:
        return f"{bps / 1_000_000:.1f} Mbps"
    if abs(bps) >= 1_000:
        return f"{bps / 1_000:.1f} kbps"
    return f"{bps:.0f} bps"


def fmt_pps(pps):
    if abs(pps) >= 1000:
        return f"{pps / 1000:.1f}k/s"
    return f"{pps:.0f}/s"


def ema(prev, cur, alpha=EMA_ALPHA):
    if prev is None:
        return cur
    return alpha * cur + (1 - alpha) * prev


STATE_NAMES = {0: "DISABLED", 1: "IDLE", 2: "BINDING", 3: "ACTIVE"}
STATE_COLORS = {0: 1, 1: 3, 2: 3, 3: 2}  # 1=red, 2=green, 3=yellow


def safe_addstr(stdscr, row, col, text, *args):
    h, w = stdscr.getmaxyx()
    if row >= h - 1 or col >= w:
        return
    text = text[:w - col - 1]
    try:
        stdscr.addstr(row, col, text, *args)
    except curses.error:
        pass


# ---- stdout --status mode ----------------------------------------------------

def print_status():
    """One-shot status dump to stdout."""
    primary, helper = find_coop_ifaces()
    if not primary:
        print("No primary interface found — is module loaded with rtw_cooperative_rx=1?")
        sys.exit(1)

    info = parse_info(primary)
    stats = parse_stats()

    state_name = info.get("state_name", "?")
    bssid = info.get("bssid", "?")
    channel = info.get("channel", "?")
    bw = info.get("bw", "0")
    drop_primary = info.get("drop_primary", "0")

    # Primary info
    pri_iface = info.get("primary_iface", primary)
    pri_ch = info.get("primary_channel", "?")
    pri_rssi = info.get("primary_rssi", "?")
    pri_signal = info.get("primary_signal", "?")
    pri_ssid = info.get("primary_ssid", "")
    pri_connected = info.get("primary_connected", "0") == "1"

    # Helper info
    hlp_iface = info.get("helper0_iface", helper or "none")
    hlp_ch = info.get("helper0_channel", "?")
    hlp_rssi = info.get("helper0_rssi", "?")
    hlp_signal = info.get("helper0_signal", "?")

    pri_mode = info.get("primary_mode", "STA")
    print(f"=== Cooperative RX Diversity ({pri_mode} mode) ===")
    pri_ch_tag = ""
    if pri_ch != channel:
        pri_ch_tag = f"  (pri: {pri_ch}) MISMATCH"
    else:
        pri_ch_tag = f"  (pri: {pri_ch})"
    print(f"State:   {state_name}  BSSID: {bssid}  Ch: {channel}{pri_ch_tag}  BW: {bw}")
    if drop_primary == "1":
        print(f"         drop_primary: ON")
    print()

    # Primary
    pri_mode = info.get("primary_mode", "STA")
    if pri_mode == "AP":
        status = "AP RUNNING" if pri_connected else "AP DOWN"
    else:
        status = "CONNECTED" if pri_connected else "DOWN"
    ssid_part = f"  {pri_ssid}" if pri_ssid else ""
    rssi_part = f"  {pri_rssi} dBm (signal {pri_signal}%)" if pri_rssi != "?" else ""
    print(f"  PRIMARY  {pri_iface:<24s} [{status}] ch={pri_ch}{rssi_part}{ssid_part}")

    # Helper
    if hlp_iface and hlp_iface != "none":
        rssi_part = ""
        if hlp_rssi != "?" and hlp_rssi != "0":
            rssi_part = f"  {hlp_rssi} dBm (signal {hlp_signal}%)"
        # Check helper link state via interface flags (IFF_UP = 0x1)
        hlp_link = "?"
        hlp_flags_str = read_file(f"/sys/class/net/{hlp_iface}/flags")
        if hlp_flags_str:
            hlp_flags = int(hlp_flags_str, 16)
            hlp_link = "UP" if (hlp_flags & 0x1) else "DOWN"
        print(f"  HELPER   {hlp_iface:<24s} [monitor {hlp_link}]  ch={hlp_ch}{rssi_part}")
    else:
        print(f"  HELPER   (none paired)")

    if stats:
        print()
        accepted = stats.get("helper_rx_accepted", 0)
        dup = stats.get("helper_rx_dup_dropped", 0)
        candidates = stats.get("helper_rx_candidates", 0)
        foreign = stats.get("helper_rx_foreign", 0)
        crypto = stats.get("helper_rx_crypto_err", 0)
        late = stats.get("helper_rx_late", 0)
        deferred = stats.get("helper_rx_deferred", 0)
        pending = stats.get("pending_count", 0)
        backpressure = stats.get("helper_rx_backpressure", 0)

        decided = accepted + dup
        contrib = (accepted / decided * 100) if decided > 0 else 0

        rssi_better = stats.get("helper_rx_rssi_better", 0)
        rssi_worse = stats.get("helper_rx_rssi_worse", 0)

        print(f"  Candidates:  {candidates:>8,}  (foreign: {foreign:,})")
        print(f"  Accepted:    {accepted:>8,}")
        print(f"  Dup dropped: {dup:>8,}")
        print(f"  Late:        {late:>8,}")
        print(f"  Crypto err:  {crypto:>8,}")
        print(f"  Deferred:    {deferred:>8,}  pending: {pending}  backpressure: {backpressure}")
        if accepted > 0:
            print(f"  RSSI:        better={rssi_better:,}  worse={rssi_worse:,}")
        if decided > 0:
            print(f"  Helper contribution: {contrib:.1f}%  ({accepted:,} / {decided:,} decided)")


# ---- curses TUI mode ---------------------------------------------------------

def draw(stdscr):
    curses.curs_set(0)
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_RED, -1)
    curses.init_pair(2, curses.COLOR_GREEN, -1)
    curses.init_pair(3, curses.COLOR_YELLOW, -1)
    curses.init_pair(4, curses.COLOR_CYAN, -1)
    curses.init_pair(5, curses.COLOR_WHITE, -1)
    curses.init_pair(6, curses.COLOR_MAGENTA, -1)
    BOLD = curses.A_BOLD
    DIM = curses.A_DIM

    stdscr.nodelay(True)

    prev_stats = None
    prev_time = None
    prev_net = {}
    flash_msg = ""
    flash_until = 0.0
    rates = {}

    while True:
        primary, helper = find_coop_ifaces()

        try:
            key = stdscr.getch()
            if key in (ord("q"), ord("Q"), 27):
                break
            elif key in (ord("r"), ord("R")):
                if primary:
                    path = f"/sys/class/net/{primary}/coop_rx/coop_rx_reset_stats"
                    if write_file(path, "1"):
                        flash_msg = "Stats reset"
                        prev_stats = None
                        rates.clear()
                    else:
                        flash_msg = "Reset failed (permission?)"
                    flash_until = time.monotonic() + 2.0
            elif key in (ord("d"), ord("D")):
                if primary:
                    path = f"/sys/class/net/{primary}/coop_rx/coop_rx_drop_primary"
                    cur = read_file(path)
                    new_val = "0" if cur and cur.strip() == "1" else "1"
                    if write_file(path, new_val):
                        flash_msg = f"drop_primary = {new_val}"
                    else:
                        flash_msg = "Toggle failed (permission?)"
                    flash_until = time.monotonic() + 2.0
        except curses.error:
            pass

        stats = parse_stats()
        now = time.monotonic()
        dt = (now - prev_time) if prev_time else 0

        # Read coop_rx_info from driver sysfs
        info = parse_info(primary) if primary else {}

        # Read interface byte counters
        cur_net = {}
        for iface in (primary, helper):
            if iface:
                base = f"/sys/class/net/{iface}/statistics"
                cur_net[iface] = {}
                for name in ("rx_bytes", "rx_packets"):
                    val = read_file(f"{base}/{name}")
                    cur_net[iface][name] = int(val) if val else 0

        # Calculate rates
        if stats and prev_stats and dt > 0:
            for counter in ("helper_rx_candidates", "helper_rx_accepted",
                            "helper_rx_dup_dropped", "helper_rx_foreign",
                            "helper_rx_crypto_err", "helper_rx_late",
                            "helper_rx_no_sta", "helper_rx_pool_full",
                            "helper_rx_deferred", "helper_rx_backpressure"):
                raw = (stats.get(counter, 0) - prev_stats.get(counter, 0)) / dt
                rates[counter] = ema(rates.get(counter), raw)

        for iface in (primary, helper):
            if iface and iface in cur_net and iface in prev_net and dt > 0:
                rx_bps = (cur_net[iface]["rx_bytes"] - prev_net[iface]["rx_bytes"]) * 8 / dt
                rates[f"{iface}_rx_bps"] = ema(rates.get(f"{iface}_rx_bps"), rx_bps)

        prev_stats = stats
        prev_time = now
        prev_net = cur_net

        # Read drop_primary state from info
        drop_primary = info.get("drop_primary", "0") == "1"

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        if h < 28 or w < 72:
            safe_addstr(stdscr, 0, 0, f"Terminal too small ({w}x{h}, need 72x28)")
            stdscr.refresh()
            time.sleep(0.5)
            continue

        row = 0
        W = min(w - 1, 78)

        # Title bar
        title = " Cooperative RX Diversity Monitor "
        safe_addstr(stdscr, row, max(0, (W - len(title)) // 2), title,
                    BOLD | curses.color_pair(4))
        row += 1
        safe_addstr(stdscr, row, 0, "\u2500" * W, curses.color_pair(5) | DIM)
        row += 1

        if not stats:
            safe_addstr(stdscr, row, 2,
                        "No data \u2014 is module loaded with rtw_cooperative_rx=1?",
                        curses.color_pair(1))
            stdscr.refresh()
            time.sleep(1.0 / REFRESH_HZ)
            continue

        # === State line (from driver sysfs info) ===
        state_num = stats.get("state", 0)
        state_name = info.get("state_name", STATE_NAMES.get(state_num, "?"))
        state_color = STATE_COLORS.get(state_num, 5)
        pri_mode = info.get("primary_mode", "STA")
        safe_addstr(stdscr, row, 2, "State: ", BOLD)
        safe_addstr(stdscr, row, 9, state_name,
                    BOLD | curses.color_pair(state_color))
        col = 9 + len(state_name) + 1
        safe_addstr(stdscr, row, col, f"({pri_mode})",
                    curses.color_pair(4))
        col += len(pri_mode) + 3
        bssid = info.get("bssid", stats.get("bound_bssid", "\u2014"))
        ch = info.get("channel", str(stats.get("bound_channel", "\u2014")))
        pri_ch = info.get("primary_channel", ch)
        ch_str = f"BSSID: {bssid}  Ch: {ch}"
        safe_addstr(stdscr, row, col, ch_str)
        pcol = col + len(ch_str) + 1
        if str(pri_ch) != str(ch):
            safe_addstr(stdscr, row, pcol, f"(pri: {pri_ch}) MISMATCH",
                        BOLD | curses.color_pair(1))
        else:
            safe_addstr(stdscr, row, pcol, f"(pri: {pri_ch})",
                        curses.color_pair(5) | DIM)

        if drop_primary:
            dp_str = "  drop_primary: ON"
            safe_addstr(stdscr, row, W - len(dp_str),
                        dp_str, BOLD | curses.color_pair(1))
        row += 2

        # === Interfaces (from driver sysfs info — no iw needed) ===
        safe_addstr(stdscr, row, 2, "Interfaces", BOLD | curses.color_pair(4))
        row += 1

        if primary:
            pri_mode = info.get("primary_mode", "STA")
            pri_connected = info.get("primary_connected", "0") == "1"
            if pri_mode == "AP":
                status = "AP RUNNING" if pri_connected else "AP DOWN"
            else:
                status = "CONNECTED" if pri_connected else "DOWN"
            sc = 2 if pri_connected else 1
            safe_addstr(stdscr, row, 3, "PRIMARY", BOLD)
            safe_addstr(stdscr, row, 12, info.get("primary_iface", primary))
            safe_addstr(stdscr, row, 33, f"[{status}]",
                        curses.color_pair(sc))
            if pri_connected:
                rssi = info.get("primary_rssi", "?")
                ssid = info.get("primary_ssid", "")
                safe_addstr(stdscr, row, 45, f"{rssi} dBm  {ssid}")
            row += 1

        if helper:
            hlp_ch = info.get("helper0_channel", "?")
            bound_ch = info.get("channel", "?")
            hcolor = 2 if hlp_ch == bound_ch else 3
            # Check helper link state via interface flags (IFF_UP = 0x1)
            hlp_flags_str = read_file(f"/sys/class/net/{helper}/flags")
            if hlp_flags_str:
                hlp_up = bool(int(hlp_flags_str, 16) & 0x1)
            else:
                hlp_up = False
            hlp_state_str = "UP" if hlp_up else "DOWN"
            hlp_state_color = 2 if hlp_up else 1
            safe_addstr(stdscr, row, 3, "HELPER", BOLD)
            safe_addstr(stdscr, row, 12, info.get("helper0_iface", helper))
            safe_addstr(stdscr, row, 33, f"[monitor {hlp_state_str}]",
                        curses.color_pair(hlp_state_color))
            safe_addstr(stdscr, row, 48, f"ch={hlp_ch}")
            hlp_rssi = info.get("helper0_rssi", "0")
            if hlp_rssi != "0" and hlp_rssi != "?":
                safe_addstr(stdscr, row, 58, f"{hlp_rssi} dBm")
        elif stats.get("num_helpers", 0) > 0:
            safe_addstr(stdscr, row, 3, "HELPER", BOLD)
            safe_addstr(stdscr, row, 12, "(paired, interface not found)",
                        curses.color_pair(3))
        else:
            safe_addstr(stdscr, row, 3, "HELPER", BOLD)
            safe_addstr(stdscr, row, 12, "(none paired)",
                        curses.color_pair(1))
        row += 2

        # === Throughput ===
        safe_addstr(stdscr, row, 2, "Throughput", BOLD | curses.color_pair(4))
        safe_addstr(stdscr, row, 50, "rate", DIM)
        safe_addstr(stdscr, row, 63, "total", DIM)
        row += 1

        if primary and primary in cur_net:
            rx_b = cur_net[primary]["rx_bytes"]
            rx_rate = rates.get(f"{primary}_rx_bps", 0)
            safe_addstr(stdscr, row, 3, "Primary stack RX")
            safe_addstr(stdscr, row, 46,
                        f"{fmt_rate(rx_rate):>10s}", BOLD | curses.color_pair(2))
            safe_addstr(stdscr, row, 59,
                        f"{fmt_bytes(rx_b):>10s}")
            row += 1
            safe_addstr(stdscr, row, 3,
                        "(primary radio + helper injected frames)",
                        DIM | curses.color_pair(5))
            row += 2

        # === Helper RX Pipeline ===
        safe_addstr(stdscr, row, 2, "Helper RX Pipeline",
                    BOLD | curses.color_pair(4))
        safe_addstr(stdscr, row, 50, "/sec", DIM)
        safe_addstr(stdscr, row, 63, "total", DIM)
        row += 1

        candidates = stats.get("helper_rx_candidates", 0)
        accepted = stats.get("helper_rx_accepted", 0)
        dup = stats.get("helper_rx_dup_dropped", 0)
        foreign = stats.get("helper_rx_foreign", 0)
        crypto = stats.get("helper_rx_crypto_err", 0)
        late = stats.get("helper_rx_late", 0)
        no_sta = stats.get("helper_rx_no_sta", 0)
        pool_full = stats.get("helper_rx_pool_full", 0)
        deferred = stats.get("helper_rx_deferred", 0)
        backpressure = stats.get("helper_rx_backpressure", 0)
        pending = stats.get("pending_count", 0)
        matched = candidates - foreign

        def stat_line(label, value, rate_key, color=5, indent=3):
            nonlocal row
            r = rates.get(rate_key, 0)
            safe_addstr(stdscr, row, indent, f"{label:<34s}")
            safe_addstr(stdscr, row, 46,
                        f"{fmt_pps(r):>10s}" if r else f"{'—':>10s}",
                        curses.color_pair(color) if r else DIM)
            safe_addstr(stdscr, row, 59,
                        f"{value:>10,}", curses.color_pair(color))
            row += 1

        stat_line("Candidates (all helper data)",
                  candidates, "helper_rx_candidates")
        stat_line("\u251c Foreign (wrong BSS/direction)",
                  foreign, "helper_rx_foreign")

        matched_rate = rates.get("helper_rx_candidates", 0) - rates.get("helper_rx_foreign", 0)
        safe_addstr(stdscr, row, 3, f"{'\u251c BSSID matched':<34s}")
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(matched_rate):>10s}" if matched_rate else f"{'—':>10s}",
                    curses.color_pair(5) if matched_rate else DIM)
        safe_addstr(stdscr, row, 59, f"{matched:>10,}")
        row += 1

        ac = 2 if accepted > 0 else 5
        safe_addstr(stdscr, row, 3, f"{'  \u251c Accepted (injected \u2192 stack)':<34s}")
        r_acc = rates.get("helper_rx_accepted", 0)
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(r_acc):>10s}" if r_acc else f"{'—':>10s}",
                    curses.color_pair(ac))
        safe_addstr(stdscr, row, 59,
                    f"{accepted:>10,}",
                    BOLD | curses.color_pair(2))
        row += 1

        dc = 3 if dup > 0 else 2
        safe_addstr(stdscr, row, 3, f"{'  \u251c Dup dropped (primary won race)':<34s}")
        r_dup = rates.get("helper_rx_dup_dropped", 0)
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(r_dup):>10s}" if r_dup else f"{'—':>10s}",
                    curses.color_pair(dc))
        safe_addstr(stdscr, row, 59,
                    f"{dup:>10,}", curses.color_pair(dc))
        row += 1

        lc = 3 if late > 0 else 2
        safe_addstr(stdscr, row, 3, f"{'  \u251c Late (past reorder window)':<34s}")
        r_late = rates.get("helper_rx_late", 0)
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(r_late):>10s}" if r_late else f"{'—':>10s}",
                    curses.color_pair(lc))
        safe_addstr(stdscr, row, 59,
                    f"{late:>10,}", curses.color_pair(lc))
        row += 1

        cc = 2 if crypto == 0 else 1
        safe_addstr(stdscr, row, 3, f"{'  \u251c Crypto errors (decrypt fail)':<34s}")
        r_cry = rates.get("helper_rx_crypto_err", 0)
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(r_cry):>10s}" if r_cry else f"{'—':>10s}",
                    curses.color_pair(cc))
        safe_addstr(stdscr, row, 59,
                    f"{crypto:>10,}", curses.color_pair(cc))
        row += 1

        nc = 3 if no_sta > 0 else 2
        safe_addstr(stdscr, row, 3, f"{'  \u2514 Pool full / No STA':<34s}")
        safe_addstr(stdscr, row, 59,
                    f"{pool_full + no_sta:>10,}", curses.color_pair(nc))
        row += 2

        # === Deferred Queue ===
        safe_addstr(stdscr, row, 2, "Deferred Queue",
                    BOLD | curses.color_pair(4))
        row += 1

        pc = 2 if pending == 0 else (3 if pending < 64 else 1)
        safe_addstr(stdscr, row, 3, f"{'Pending frames:':<24s}")
        safe_addstr(stdscr, row, 27, f"{pending}",
                    BOLD | curses.color_pair(pc))
        row += 1

        r_def = rates.get("helper_rx_deferred", 0)
        safe_addstr(stdscr, row, 3, f"{'Deferred (enqueued):':<24s}")
        safe_addstr(stdscr, row, 27, f"{deferred:,}")
        if r_def:
            safe_addstr(stdscr, row, 42, f"({fmt_pps(r_def)})",
                        curses.color_pair(4))
        row += 1

        bpc = 2 if backpressure == 0 else 1
        r_bp = rates.get("helper_rx_backpressure", 0)
        safe_addstr(stdscr, row, 3, f"{'Backpressure drops:':<24s}")
        safe_addstr(stdscr, row, 27, f"{backpressure:,}",
                    curses.color_pair(bpc))
        if r_bp:
            safe_addstr(stdscr, row, 42, f"({fmt_pps(r_bp)})",
                        curses.color_pair(1))
        row += 2

        # === Diversity Metrics ===
        safe_addstr(stdscr, row, 2, "Diversity Metrics",
                    BOLD | curses.color_pair(4))
        row += 1

        decided = accepted + dup
        if decided > 0:
            win_pct = accepted / decided * 100
            dup_pct = dup / decided * 100
            wc = 2 if win_pct > 50 else (3 if win_pct > 10 else 1)
            safe_addstr(stdscr, row, 3, "Helper contribution:")
            safe_addstr(stdscr, row, 25,
                        f"{win_pct:5.1f}%",
                        BOLD | curses.color_pair(wc))
            safe_addstr(stdscr, row, 35,
                        f"novel  ({accepted:,} / {decided:,} frames)",
                        DIM)
            row += 1

            safe_addstr(stdscr, row, 3, "Dup overlap:")
            safe_addstr(stdscr, row, 25, f"{dup_pct:5.1f}%")
            safe_addstr(stdscr, row, 35,
                        f"redundant  (primary already had frame)",
                        DIM)
            row += 1

            # Instantaneous contribution rate
            r_acc = rates.get("helper_rx_accepted", 0)
            r_dup = rates.get("helper_rx_dup_dropped", 0)
            r_total = r_acc + r_dup
            if r_total > 1:
                inst_pct = r_acc / r_total * 100
                ic = 2 if inst_pct > 50 else (3 if inst_pct > 10 else 1)
                safe_addstr(stdscr, row, 3, "  (current rate:")
                safe_addstr(stdscr, row, 19,
                            f"{inst_pct:5.1f}%",
                            curses.color_pair(ic))
                safe_addstr(stdscr, row, 26,
                            f"= {fmt_pps(r_acc)} novel / {fmt_pps(r_total)} decided)",
                            DIM)
                row += 1
        else:
            safe_addstr(stdscr, row, 3, "(no frames processed yet)", DIM)
            row += 1

        if matched > 0:
            crypto_health = (1 - crypto / matched) * 100
            ch_c = 2 if crypto_health >= 99.9 else (3 if crypto_health > 90 else 1)
            safe_addstr(stdscr, row, 3, "Decrypt health:")
            safe_addstr(stdscr, row, 25,
                        f"{crypto_health:5.1f}%",
                        BOLD | curses.color_pair(ch_c))
            if crypto > 0:
                safe_addstr(stdscr, row, 35,
                            f"({crypto} failures)",
                            curses.color_pair(1))
            row += 1
        row += 1

        # === Events ===
        safe_addstr(stdscr, row, 2, "Events", BOLD | curses.color_pair(4))
        row += 1
        safe_addstr(stdscr, row, 3,
                    f"Pair: {stats.get('pair_events', 0)}   "
                    f"Unpair: {stats.get('unpair_events', 0)}   "
                    f"Fallback: {stats.get('fallback_events', 0)}")
        row += 1

        # === Footer ===
        row += 1
        safe_addstr(stdscr, row, 0, "\u2500" * W,
                    curses.color_pair(5) | DIM)
        row += 1

        if flash_msg and time.monotonic() < flash_until:
            safe_addstr(stdscr, row, 2, flash_msg,
                        BOLD | curses.color_pair(3))
        else:
            flash_msg = ""
            safe_addstr(stdscr, row, 2,
                        "q=quit  r=reset stats  d=toggle drop_primary",
                        curses.color_pair(5) | DIM)

        stdscr.refresh()
        time.sleep(1.0 / REFRESH_HZ)


def main():
    if os.geteuid() != 0:
        print("Must run as root (needs debugfs access)")
        sys.exit(1)

    if "--status" in sys.argv or "-s" in sys.argv:
        print_status()
        return

    if not os.path.exists(STATS_PATH):
        print(f"Stats not found at {STATS_PATH}")
        print("Is the driver loaded with rtw_cooperative_rx=1?")
        sys.exit(1)
    curses.wrapper(draw)


if __name__ == "__main__":
    main()
