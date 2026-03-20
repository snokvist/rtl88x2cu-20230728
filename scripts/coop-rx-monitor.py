#!/usr/bin/env python3
"""
coop-rx-monitor — Interactive monitor for RTL8822CU cooperative RX diversity

Reads /sys/kernel/debug/rtw_coop_rx/stats and network interface counters
to display a live dashboard of cooperative RX performance.

Usage: sudo python3 coop-rx-monitor.py

Keys:
  q / ESC  — quit
  r        — reset cooperative RX stats
  d        — toggle drop_primary (helper-only test mode)
"""

import curses
import time
import os
import subprocess
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


def get_iface_stats(iface):
    """Read kernel network interface byte/packet counters (stack-level)."""
    base = f"/sys/class/net/{iface}/statistics"
    result = {}
    for name in ("rx_bytes", "rx_packets", "tx_bytes", "tx_packets"):
        val = read_file(f"{base}/{name}")
        result[name] = int(val) if val else 0
    return result


def get_iface_info(iface):
    """Get link info from iw dev."""
    info = {"connected": False, "ssid": "", "freq": "", "signal": "",
            "type": "managed", "channel": "",
            "rx_bitrate": "", "tx_bitrate": ""}
    try:
        out = subprocess.check_output(
            ["iw", "dev", iface, "link"], stderr=subprocess.DEVNULL, timeout=2
        ).decode()
        if "Connected" in out:
            info["connected"] = True
            for line in out.splitlines():
                line = line.strip()
                if line.startswith("SSID:"):
                    info["ssid"] = line.split(":", 1)[1].strip()
                elif "freq:" in line:
                    info["freq"] = line.split("freq:")[1].strip().split()[0]
                elif "signal:" in line:
                    info["signal"] = line.split("signal:")[1].strip()
                elif line.startswith("rx bitrate:"):
                    info["rx_bitrate"] = line.split(":", 1)[1].strip()
                elif line.startswith("tx bitrate:"):
                    info["tx_bitrate"] = line.split(":", 1)[1].strip()
    except (subprocess.SubprocessError, OSError, ValueError):
        pass
    try:
        out = subprocess.check_output(
            ["iw", "dev", iface, "info"], stderr=subprocess.DEVNULL, timeout=2
        ).decode()
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("type "):
                info["type"] = line.split()[1]
            elif line.startswith("channel "):
                info["channel"] = line.split()[1]
    except (subprocess.SubprocessError, OSError):
        pass
    if info["type"] == "monitor" and not info["channel"]:
        stats = parse_stats()
        if stats and "bound_channel" in stats:
            info["channel"] = str(stats["bound_channel"])
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
    """Exponential moving average for smooth rate display."""
    if prev is None:
        return cur
    return alpha * cur + (1 - alpha) * prev


STATE_NAMES = {0: "DISABLED", 1: "IDLE", 2: "BINDING", 3: "ACTIVE"}
STATE_COLORS = {0: 1, 1: 3, 2: 3, 3: 2}  # 1=red, 2=green, 3=yellow


def safe_addstr(stdscr, row, col, text, *args):
    """addstr that silently ignores writes beyond terminal bounds."""
    h, w = stdscr.getmaxyx()
    if row >= h - 1 or col >= w:
        return
    text = text[:w - col - 1]
    try:
        stdscr.addstr(row, col, text, *args)
    except curses.error:
        pass


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
    prev_net = {}  # {iface: {rx_bytes, rx_packets, ...}}
    flash_msg = ""
    flash_until = 0.0

    # Smoothed rates
    rates = {}

    while True:
        try:
            key = stdscr.getch()
            if key in (ord("q"), ord("Q"), 27):
                break
            elif key in (ord("r"), ord("R")):
                primary, _ = find_coop_ifaces()
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
                primary, _ = find_coop_ifaces()
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
        primary, helper = find_coop_ifaces()
        now = time.monotonic()
        dt = (now - prev_time) if prev_time else 0

        # Read interface byte counters
        cur_net = {}
        for iface in (primary, helper):
            if iface:
                cur_net[iface] = get_iface_stats(iface)

        # Calculate rates
        if stats and prev_stats and dt > 0:
            for counter in ("helper_rx_candidates", "helper_rx_accepted",
                            "helper_rx_dup_dropped", "helper_rx_foreign",
                            "helper_rx_crypto_err", "helper_rx_late",
                            "helper_rx_no_sta", "helper_rx_pool_full"):
                raw = (stats.get(counter, 0) - prev_stats.get(counter, 0)) / dt
                rates[counter] = ema(rates.get(counter), raw)

        # Interface byte rates
        for iface in (primary, helper):
            if iface and iface in cur_net and iface in prev_net and dt > 0:
                rx_bps = (cur_net[iface]["rx_bytes"] - prev_net[iface]["rx_bytes"]) * 8 / dt
                rx_pps = (cur_net[iface]["rx_packets"] - prev_net[iface]["rx_packets"]) / dt
                rates[f"{iface}_rx_bps"] = ema(rates.get(f"{iface}_rx_bps"), rx_bps)
                rates[f"{iface}_rx_pps"] = ema(rates.get(f"{iface}_rx_pps"), rx_pps)

        prev_stats = stats
        prev_time = now
        prev_net = cur_net

        # Get link-level info (less frequently — iw is slow)
        # Only refresh every ~2s to avoid iw overhead
        iw_stale = not hasattr(draw, '_iw_cache') or now - draw._iw_ts > 2.0
        if iw_stale:
            draw._iw_cache = {}
            for iface in (primary, helper):
                if iface:
                    draw._iw_cache[iface] = get_iface_info(iface)
            draw._iw_ts = now
        pri_info = draw._iw_cache.get(primary, {}) if primary else {}
        hlp_info = draw._iw_cache.get(helper, {}) if helper else {}

        # Read drop_primary state
        drop_primary = False
        if primary:
            dp = read_file(f"/sys/class/net/{primary}/coop_rx/coop_rx_drop_primary")
            drop_primary = dp and dp.strip() == "1"

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        if h < 24 or w < 72:
            safe_addstr(stdscr, 0, 0, f"Terminal too small ({w}x{h}, need 72x24)")
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

        # === State line ===
        state_num = stats.get("state", 0)
        state_name = STATE_NAMES.get(state_num, "?")
        state_color = STATE_COLORS.get(state_num, 5)
        safe_addstr(stdscr, row, 2, "State: ", BOLD)
        safe_addstr(stdscr, row, 9, state_name,
                    BOLD | curses.color_pair(state_color))
        col = 9 + len(state_name) + 2
        bssid = stats.get("bound_bssid", "\u2014")
        ch = stats.get("bound_channel", "\u2014")
        safe_addstr(stdscr, row, col,
                    f"BSSID: {bssid}  Ch: {ch}")

        # drop_primary indicator
        if drop_primary:
            dp_str = "  drop_primary: ON"
            safe_addstr(stdscr, row, W - len(dp_str),
                        dp_str, BOLD | curses.color_pair(1))
        row += 2

        # === Interfaces ===
        safe_addstr(stdscr, row, 2, "Interfaces", BOLD | curses.color_pair(4))
        row += 1

        if primary:
            connected = pri_info.get("connected", False)
            status = "CONNECTED" if connected else "DOWN"
            sc = 2 if connected else 1
            safe_addstr(stdscr, row, 3, "PRIMARY", BOLD)
            safe_addstr(stdscr, row, 12, primary)
            safe_addstr(stdscr, row, 33, f"[{status}]",
                        curses.color_pair(sc))
            if connected:
                sig = pri_info.get("signal", "")
                ssid = pri_info.get("ssid", "")
                safe_addstr(stdscr, row, 45, f"{sig}  {ssid}")
            row += 1

        if helper:
            htype = hlp_info.get("type", "?")
            hch = hlp_info.get("channel", "?")
            hcolor = 2 if htype == "monitor" and str(hch) == str(ch) else 3
            safe_addstr(stdscr, row, 3, "HELPER", BOLD)
            safe_addstr(stdscr, row, 12, helper)
            safe_addstr(stdscr, row, 33, f"[{htype}]",
                        curses.color_pair(hcolor))
            safe_addstr(stdscr, row, 45, f"ch={hch}")
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
        #                                       col 50 = rate column
        safe_addstr(stdscr, row, 50, "rate", DIM)
        safe_addstr(stdscr, row, 63, "total", DIM)
        row += 1

        if primary and primary in cur_net:
            rx_b = cur_net[primary]["rx_bytes"]
            rx_p = cur_net[primary]["rx_packets"]
            rx_rate = rates.get(f"{primary}_rx_bps", 0)
            safe_addstr(stdscr, row, 3, "Primary stack RX")
            safe_addstr(stdscr, row, 46,
                        f"{fmt_rate(rx_rate):>10s}", BOLD | curses.color_pair(2))
            safe_addstr(stdscr, row, 59,
                        f"{fmt_bytes(rx_b):>10s}")
            row += 1
            # Explain the double-counting
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

        # BSSID matched (computed, no own rate key — use candidates - foreign)
        matched_rate = rates.get("helper_rx_candidates", 0) - rates.get("helper_rx_foreign", 0)
        safe_addstr(stdscr, row, 3, f"{'\u251c BSSID matched':<34s}")
        safe_addstr(stdscr, row, 46,
                    f"{fmt_pps(matched_rate):>10s}" if matched_rate else f"{'—':>10s}",
                    curses.color_pair(5) if matched_rate else DIM)
        safe_addstr(stdscr, row, 59, f"{matched:>10,}")
        row += 1

        # Sub-items under BSSID matched
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

        # === Diversity Metrics ===
        safe_addstr(stdscr, row, 2, "Diversity Metrics",
                    BOLD | curses.color_pair(4))
        row += 1

        decided = accepted + dup
        if decided > 0:
            win_pct = accepted / decided * 100
            dup_pct = dup / decided * 100
            # Color: green >50%, yellow 10-50%, red <10%
            wc = 2 if win_pct > 50 else (3 if win_pct > 10 else 1)
            safe_addstr(stdscr, row, 3, "Helper win rate:")
            safe_addstr(stdscr, row, 25,
                        f"{win_pct:5.1f}%",
                        BOLD | curses.color_pair(wc))
            safe_addstr(stdscr, row, 35,
                        "(helper beats primary to stack)",
                        DIM)
            row += 1

            safe_addstr(stdscr, row, 3, "Dup overlap:")
            safe_addstr(stdscr, row, 25, f"{dup_pct:5.1f}%")
            safe_addstr(stdscr, row, 35,
                        "(primary already had the frame)",
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

        # Flash message or default help
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
    if not os.path.exists(STATS_PATH):
        print(f"Stats not found at {STATS_PATH}")
        print("Is the driver loaded with rtw_cooperative_rx=1?")
        sys.exit(1)
    curses.wrapper(draw)


if __name__ == "__main__":
    main()
