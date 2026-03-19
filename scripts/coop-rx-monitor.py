#!/usr/bin/env python3
"""
coop-rx-monitor — Interactive monitor for RTL8822CU cooperative RX diversity

Reads /sys/kernel/debug/rtw_coop_rx/stats and network interface info
to display a live dashboard of cooperative RX performance.

Usage: sudo python3 coop-rx-monitor.py
"""

import curses
import time
import os
import subprocess
import sys

STATS_PATH = "/sys/kernel/debug/rtw_coop_rx/stats"
REFRESH_HZ = 4  # updates per second


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except (OSError, PermissionError):
        return None


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
            # Handle MAC format (contains colons)
            if key.strip() in ("bound_bssid",):
                stats[key.strip()] = val
            else:
                # Try to extract just the number (ignore state name in parens)
                num = val.split()[0] if val else val
                try:
                    stats[key.strip()] = int(num)
                except ValueError:
                    stats[key.strip()] = val
    return stats


def get_iface_info(iface):
    """Get link info from iw dev"""
    info = {"connected": False, "ssid": "", "freq": "", "signal": "",
            "rx_bytes": 0, "rx_packets": 0, "tx_bytes": 0, "tx_packets": 0,
            "type": "managed", "channel": ""}
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
                elif line.startswith("RX:"):
                    parts = line.split()
                    info["rx_bytes"] = int(parts[1])
                    info["rx_packets"] = int(parts[3].strip("("))
                elif line.startswith("TX:"):
                    parts = line.split()
                    info["tx_bytes"] = int(parts[1])
                    info["tx_packets"] = int(parts[3].strip("("))
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
    # For monitor mode helpers, cfg80211 doesn't track the real channel.
    # Read it from the cooperative RX stats (bound_channel) instead.
    if info["type"] == "monitor" and not info["channel"]:
        stats = parse_stats()
        if stats and "bound_channel" in stats:
            info["channel"] = str(stats["bound_channel"])
    return info


def find_coop_ifaces():
    """Find primary and helper interfaces"""
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
    if bps >= 1_000_000:
        return f"{bps / 1_000_000:.1f} Mbps"
    if bps >= 1_000:
        return f"{bps / 1_000:.1f} Kbps"
    return f"{bps:.0f} bps"


STATE_NAMES = {0: "DISABLED", 1: "IDLE", 2: "BINDING", 3: "ACTIVE", 4: "TEARDOWN"}
STATE_COLORS = {0: 1, 1: 3, 2: 3, 3: 2, 4: 1}  # 1=red, 2=green, 3=yellow


def draw(stdscr):
    curses.curs_set(0)
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_RED, -1)
    curses.init_pair(2, curses.COLOR_GREEN, -1)
    curses.init_pair(3, curses.COLOR_YELLOW, -1)
    curses.init_pair(4, curses.COLOR_CYAN, -1)
    curses.init_pair(5, curses.COLOR_WHITE, -1)
    curses.init_pair(6, curses.COLOR_MAGENTA, -1)

    stdscr.nodelay(True)

    prev_stats = None
    prev_time = None
    prev_rx_bytes = None
    rates = {"accepted_rate": 0.0, "candidates_rate": 0.0, "rx_rate": 0.0}

    while True:
        try:
            key = stdscr.getch()
            if key in (ord("q"), ord("Q"), 27):
                break
        except curses.error:
            pass

        stats = parse_stats()
        primary, helper = find_coop_ifaces()
        now = time.monotonic()

        # Calculate rates
        if stats and prev_stats and prev_time:
            dt = now - prev_time
            if dt > 0:
                rates["accepted_rate"] = (
                    stats.get("helper_rx_accepted", 0) -
                    prev_stats.get("helper_rx_accepted", 0)
                ) / dt
                rates["candidates_rate"] = (
                    stats.get("helper_rx_candidates", 0) -
                    prev_stats.get("helper_rx_candidates", 0)
                ) / dt

        # Get interface info
        pri_info = get_iface_info(primary) if primary else {}
        hlp_info = get_iface_info(helper) if helper else {}

        if pri_info and prev_rx_bytes is not None and prev_time:
            dt = now - prev_time
            if dt > 0:
                rates["rx_rate"] = (pri_info.get("rx_bytes", 0) - prev_rx_bytes) * 8 / dt

        prev_stats = stats
        prev_time = now
        prev_rx_bytes = pri_info.get("rx_bytes", 0) if pri_info else None

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        if h < 20 or w < 60:
            stdscr.addstr(0, 0, "Terminal too small (need 60x20)")
            stdscr.refresh()
            time.sleep(0.5)
            continue

        row = 0

        # Title
        title = " Cooperative RX Diversity Monitor "
        stdscr.addstr(row, max(0, (w - len(title)) // 2), title,
                      curses.A_BOLD | curses.color_pair(4))
        row += 1
        stdscr.addstr(row, 0, "─" * min(w - 1, 72), curses.color_pair(5))
        row += 1

        if not stats:
            stdscr.addstr(row, 2, "No cooperative RX data — is the module loaded with rtw_cooperative_rx=1?",
                          curses.color_pair(1))
            stdscr.refresh()
            time.sleep(1.0 / REFRESH_HZ)
            continue

        # State
        state_num = stats.get("state", 0)
        state_name = STATE_NAMES.get(state_num, "?")
        state_color = STATE_COLORS.get(state_num, 5)
        stdscr.addstr(row, 2, "State: ", curses.A_BOLD)
        stdscr.addstr(f"{state_name}", curses.A_BOLD | curses.color_pair(state_color))
        bssid = stats.get("bound_bssid", "—")
        ch = stats.get("bound_channel", "—")
        stdscr.addstr(f"   BSSID: {bssid}   Channel: {ch}")
        row += 2

        # Interfaces
        stdscr.addstr(row, 2, "Interfaces", curses.A_BOLD | curses.color_pair(4))
        row += 1

        if primary:
            status = "CONNECTED" if pri_info.get("connected") else "DOWN"
            sc = 2 if pri_info.get("connected") else 1
            stdscr.addstr(row, 4, f"Primary  {primary:<20s}", curses.A_BOLD)
            stdscr.addstr(f" [{status}]", curses.color_pair(sc))
            if pri_info.get("connected"):
                stdscr.addstr(f"  {pri_info.get('signal', '')}  {pri_info.get('ssid', '')}")
            row += 1

        if helper:
            htype = hlp_info.get("type", "?")
            hch = hlp_info.get("channel", "?")
            hcolor = 2 if htype == "monitor" and str(hch) == str(ch) else 3
            stdscr.addstr(row, 4, f"Helper   {helper:<20s}", curses.A_BOLD)
            stdscr.addstr(f" [{htype}]", curses.color_pair(hcolor))
            stdscr.addstr(f"  ch={hch}")
        elif stats.get("num_helpers", 0) > 0:
            stdscr.addstr(row, 4, "Helper   (paired but interface not found)", curses.color_pair(3))
        else:
            stdscr.addstr(row, 4, "Helper   (none paired)", curses.color_pair(1))
        row += 2

        # Traffic
        stdscr.addstr(row, 2, "Traffic", curses.A_BOLD | curses.color_pair(4))
        row += 1
        if pri_info:
            stdscr.addstr(row, 4, f"Primary RX: {fmt_bytes(pri_info.get('rx_bytes', 0)):>10s}"
                          f"  ({pri_info.get('rx_packets', 0):,} pkts)")
            if rates["rx_rate"] > 0:
                stdscr.addstr(f"   {fmt_rate(rates['rx_rate']):>12s}", curses.color_pair(2))
        row += 2

        # Cooperative RX counters
        stdscr.addstr(row, 2, "Cooperative RX Counters", curses.A_BOLD | curses.color_pair(4))
        row += 1

        candidates = stats.get("helper_rx_candidates", 0)
        accepted = stats.get("helper_rx_accepted", 0)
        dup = stats.get("helper_rx_dup_dropped", 0)
        pool_full = stats.get("helper_rx_pool_full", 0)
        foreign = stats.get("helper_rx_foreign", 0)
        crypto = stats.get("helper_rx_crypto_err", 0)
        late = stats.get("helper_rx_late", 0)
        no_sta = stats.get("helper_rx_no_sta", 0)

        # Acceptance ratio
        bssid_matched = candidates - foreign
        accept_pct = (accepted / bssid_matched * 100) if bssid_matched > 0 else 0

        stdscr.addstr(row, 4, f"{'Candidates:':<22s}")
        stdscr.addstr(f"{candidates:>8,}", curses.A_BOLD)
        if rates["candidates_rate"] > 0:
            stdscr.addstr(f"  ({rates['candidates_rate']:.1f}/s)", curses.color_pair(5))
        row += 1

        stdscr.addstr(row, 4, f"{'Accepted:':<22s}")
        stdscr.addstr(f"{accepted:>8,}", curses.A_BOLD | curses.color_pair(2))
        if rates["accepted_rate"] > 0:
            stdscr.addstr(f"  ({rates['accepted_rate']:.1f}/s)", curses.color_pair(2))
        if bssid_matched > 0:
            stdscr.addstr(f"  [{accept_pct:.0f}% of matched]", curses.color_pair(5))
        row += 1

        stdscr.addstr(row, 4, f"{'Dup dropped:':<22s}")
        dc = 2 if dup == 0 else 3
        stdscr.addstr(f"{dup:>8,}", curses.color_pair(dc))
        row += 1

        stdscr.addstr(row, 4, f"{'Pool full:':<22s}")
        pc = 2 if pool_full == 0 else 1
        stdscr.addstr(f"{pool_full:>8,}", curses.color_pair(pc))
        row += 1

        stdscr.addstr(row, 4, f"{'Foreign BSSID:':<22s}")
        stdscr.addstr(f"{foreign:>8,}", curses.color_pair(5))
        row += 1

        stdscr.addstr(row, 4, f"{'Crypto errors:':<22s}")
        cc = 2 if crypto == 0 else 1
        stdscr.addstr(f"{crypto:>8,}", curses.color_pair(cc))
        row += 1

        stdscr.addstr(row, 4, f"{'Late (past window):':<22s}")
        lc = 2 if late == 0 else 3
        stdscr.addstr(f"{late:>8,}", curses.color_pair(lc))
        row += 1

        stdscr.addstr(row, 4, f"{'No STA info:':<22s}")
        nc = 2 if no_sta == 0 else 3
        stdscr.addstr(f"{no_sta:>8,}", curses.color_pair(nc))
        row += 2

        # Events
        stdscr.addstr(row, 2, "Events", curses.A_BOLD | curses.color_pair(4))
        row += 1
        stdscr.addstr(row, 4,
                      f"Pair: {stats.get('pair_events', 0)}   "
                      f"Unpair: {stats.get('unpair_events', 0)}   "
                      f"Fallback: {stats.get('fallback_events', 0)}")
        row += 2

        # Footer
        stdscr.addstr(row, 0, "─" * min(w - 1, 72), curses.color_pair(5))
        row += 1
        stdscr.addstr(row, 2, "q=quit", curses.color_pair(5))

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
