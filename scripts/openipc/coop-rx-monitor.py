#!/usr/bin/env python3
"""
coop-rx-monitor — Cooperative RX monitor for OpenIPC/BusyBox targets

Lightweight version of the full monitor for resource-constrained systems.
Falls back to plain terminal output if curses is unavailable.

Usage:
  python3 coop-rx-monitor.py           # live dashboard (curses if available)
  python3 coop-rx-monitor.py --status  # one-shot summary
  python3 coop-rx-monitor.py --watch   # plain-text live (no curses needed)

Keys (curses mode):
  q / ESC  — quit
  r        — reset stats
  d        — toggle drop_primary
"""

import time
import os
import sys

STATS_PATH = "/sys/kernel/debug/rtw_coop_rx/stats"
REFRESH_HZ = 2  # lower for embedded targets


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
        if ":" in line and not line.startswith("==="):
            key, _, val = line.partition(":")
            val = val.strip().split()[0] if val.strip() else val
            try:
                stats[key.strip()] = int(val)
            except ValueError:
                stats[key.strip()] = val
    return stats


def find_primary():
    for name in sorted(os.listdir("/sys/class/net")):
        role_path = f"/sys/class/net/{name}/coop_rx/coop_rx_role"
        role = read_file(role_path)
        if role == "primary":
            return name
    return None


def parse_info(primary):
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


# ---- One-shot status --------------------------------------------------------

def print_status():
    primary = find_primary()
    if not primary:
        print("No primary interface found")
        sys.exit(1)

    info = parse_info(primary)
    stats = parse_stats()

    state = info.get("state_name", "?")
    bssid = info.get("bssid", "?")
    ch = info.get("channel", "?")
    dp = info.get("drop_primary", "0")

    print(f"State: {state}  BSSID: {bssid}  Ch: {ch}")
    if dp == "1":
        print("  drop_primary: ON")

    if stats:
        accepted = stats.get("helper_rx_accepted", 0)
        candidates = stats.get("helper_rx_candidates", 0)
        dup = stats.get("helper_rx_dup_dropped", 0)
        pool_full = stats.get("helper_rx_pool_full", 0)
        crypto_err = stats.get("helper_rx_crypto_err", 0)
        kern_crypto = stats.get("helper_rx_kern_crypto", 0)
        helper_bytes = stats.get("helper_rx_bytes", 0)
        backpressure = stats.get("helper_rx_backpressure", 0)

        print(f"  Candidates:    {candidates:>8,}")
        print(f"  Accepted:      {accepted:>8,}")
        print(f"  Dup dropped:   {dup:>8,}")
        print(f"  Pool full:     {pool_full:>8,}")
        print(f"  Crypto errors: {crypto_err:>8,}")
        print(f"  Kern crypto:   {kern_crypto:>8,}")
        print(f"  Helper bytes:  {helper_bytes:>12,}")
        print(f"  Backpressure:  {backpressure:>8,}")

        decided = accepted + dup
        if decided > 0:
            print(f"  Contribution:  {accepted / decided * 100:.1f}%")


# ---- Plain-text watch mode --------------------------------------------------

def watch_loop():
    prev_stats = None
    prev_time = None
    prev_bytes = {}

    primary = find_primary()
    if not primary:
        print("No primary interface found")
        sys.exit(1)

    print(f"Monitoring cooperative RX on {primary} (Ctrl-C to quit)")
    print(f"{'':>3s} {'total':>8s} {'helper':>8s} {'dup':>6s} {'pool':>6s} "
          f"{'kern':>6s} {'cand/s':>8s} {'acc/s':>8s} {'helper bps':>12s}")

    while True:
        stats = parse_stats()
        now = time.monotonic()

        if stats and prev_stats and prev_time:
            dt = now - prev_time
            if dt > 0:
                # Rates
                cand_rate = (stats.get("helper_rx_candidates", 0) -
                             prev_stats.get("helper_rx_candidates", 0)) / dt
                acc_rate = (stats.get("helper_rx_accepted", 0) -
                            prev_stats.get("helper_rx_accepted", 0)) / dt
                hb_delta = (stats.get("helper_rx_bytes", 0) -
                            prev_stats.get("helper_rx_bytes", 0))
                helper_bps = hb_delta * 8 / dt if hb_delta >= 0 else 0

                # Also read kernel interface bytes for total
                rx_bytes_path = f"/sys/class/net/{primary}/statistics/rx_bytes"
                rx_b = int(read_file(rx_bytes_path) or "0")
                total_bps = 0
                if primary in prev_bytes:
                    total_bps = (rx_b - prev_bytes[primary]) * 8 / dt
                prev_bytes[primary] = rx_b

                # Use helper_bps as total when kernel counters are 0
                effective_total = total_bps if total_bps > 100 else helper_bps
                primary_bps = max(effective_total - helper_bps, 0)

                print(f"\r   {stats.get('helper_rx_accepted', 0):>8,} "
                      f"{stats.get('helper_rx_accepted', 0):>8,} "
                      f"{stats.get('helper_rx_dup_dropped', 0):>6,} "
                      f"{stats.get('helper_rx_pool_full', 0):>6,} "
                      f"{stats.get('helper_rx_kern_crypto', 0):>6,} "
                      f"{fmt_pps(cand_rate):>8s} "
                      f"{fmt_pps(acc_rate):>8s} "
                      f"{fmt_rate(helper_bps):>12s}", end="", flush=True)

        prev_stats = stats
        prev_time = now
        time.sleep(1.0 / REFRESH_HZ)


# ---- Curses TUI (if available) ----------------------------------------------

def curses_main():
    import curses

    def draw(stdscr):
        curses.curs_set(0)
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_RED, -1)
        curses.init_pair(2, curses.COLOR_GREEN, -1)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)
        curses.init_pair(4, curses.COLOR_CYAN, -1)
        BOLD = curses.A_BOLD
        DIM = curses.A_DIM

        stdscr.nodelay(True)

        prev_stats = None
        prev_time = None
        prev_bytes = {}
        rates = {}

        # EMA smoothing
        alpha = 0.3
        def ema(prev, cur):
            return cur if prev is None else alpha * cur + (1 - alpha) * prev

        primary = find_primary()

        while True:
            if not primary:
                primary = find_primary()

            try:
                key = stdscr.getch()
                if key in (ord("q"), ord("Q"), 27):
                    break
                elif key in (ord("r"), ord("R")) and primary:
                    path = f"/sys/class/net/{primary}/coop_rx/coop_rx_reset_stats"
                    write_file(path, "1")
                    prev_stats = None
                    rates.clear()
                elif key in (ord("d"), ord("D")) and primary:
                    path = f"/sys/class/net/{primary}/coop_rx/coop_rx_drop_primary"
                    cur = read_file(path)
                    write_file(path, "0" if cur and cur.strip() == "1" else "1")
            except Exception:
                pass

            stats = parse_stats()
            now = time.monotonic()
            dt = (now - prev_time) if prev_time else 0
            info = parse_info(primary) if primary else {}
            drop_primary = info.get("drop_primary", "0") == "1"

            # Compute rates
            if stats and prev_stats and dt > 0:
                for c in ("helper_rx_candidates", "helper_rx_accepted",
                          "helper_rx_dup_dropped", "helper_rx_pool_full",
                          "helper_rx_crypto_err", "helper_rx_backpressure"):
                    raw = (stats.get(c, 0) - prev_stats.get(c, 0)) / dt
                    rates[c] = ema(rates.get(c), raw)

                hb_delta = stats.get("helper_rx_bytes", 0) - prev_stats.get("helper_rx_bytes", 0)
                if hb_delta >= 0:
                    rates["helper_bps"] = ema(rates.get("helper_bps"), hb_delta * 8 / dt)

            # Kernel interface bytes for total throughput
            if primary:
                rx_b_str = read_file(f"/sys/class/net/{primary}/statistics/rx_bytes")
                rx_b = int(rx_b_str) if rx_b_str else 0
                if primary in prev_bytes and dt > 0:
                    total_bps = (rx_b - prev_bytes[primary]) * 8 / dt
                    rates["total_bps"] = ema(rates.get("total_bps"), total_bps)
                prev_bytes[primary] = rx_b

            prev_stats = stats
            prev_time = now

            # --- Draw ---
            stdscr.erase()
            h, w = stdscr.getmaxyx()
            row = 0

            def put(r, c, text, *args):
                if r < h - 1 and c < w:
                    try:
                        stdscr.addstr(r, c, text[:w - c - 1], *args)
                    except curses.error:
                        pass

            put(row, 1, " Cooperative RX Monitor ", BOLD | curses.color_pair(4))
            row += 1

            if not stats:
                put(row, 1, "No data — is module loaded with rtw_cooperative_rx=1?",
                    curses.color_pair(1))
                stdscr.refresh()
                time.sleep(1)
                continue

            # State
            state = info.get("state_name", "?")
            ch = info.get("channel", "?")
            bssid = info.get("bssid", "?")
            put(row, 1, f"State: {state}  Ch: {ch}  BSSID: {bssid}")
            if drop_primary:
                put(row, w - 20, "drop_primary: ON", BOLD | curses.color_pair(1))
            row += 2

            # Throughput
            put(row, 1, "Throughput", BOLD | curses.color_pair(4))
            row += 1

            total_bps = rates.get("total_bps", 0)
            helper_bps = rates.get("helper_bps", 0)

            if total_bps > 100:
                helper_bps = min(helper_bps, total_bps)
                primary_bps = max(total_bps - helper_bps, 0)
                effective_total = total_bps
            elif helper_bps > 100:
                primary_bps = 0
                effective_total = helper_bps
            else:
                primary_bps = 0
                effective_total = 0

            h_frac = helper_bps / effective_total if effective_total > 100 else 0
            p_frac = 1.0 - h_frac

            put(row, 2, f"Total:   {fmt_rate(effective_total):>12s}", BOLD | curses.color_pair(2))
            row += 1
            pct = f"({p_frac*100:.0f}%)" if effective_total > 100 else ""
            put(row, 2, f"Primary: {fmt_rate(primary_bps):>12s} {pct}")
            row += 1
            hpct = f"({h_frac*100:.0f}%)" if effective_total > 100 else ""
            put(row, 2, f"Helper:  {fmt_rate(helper_bps):>12s} {hpct}",
                curses.color_pair(2) if h_frac > 0.01 else DIM)
            row += 2

            # Pipeline
            put(row, 1, "Helper Pipeline", BOLD | curses.color_pair(4))
            put(row, 40, "/sec", DIM)
            put(row, 52, "total", DIM)
            row += 1

            def sline(label, key, color=0):
                nonlocal row
                r = rates.get(key, 0)
                v = stats.get(key, 0)
                put(row, 2, f"{label:<30s}")
                put(row, 37, f"{fmt_pps(r):>8s}" if r else "", curses.color_pair(color) if color else DIM)
                put(row, 49, f"{v:>10,}")
                row += 1

            sline("Candidates", "helper_rx_candidates")
            sline("Accepted", "helper_rx_accepted", 2)
            sline("Dup dropped", "helper_rx_dup_dropped", 3)
            sline("Pool full", "helper_rx_pool_full", 1 if stats.get("helper_rx_pool_full", 0) > 0 else 2)
            sline("Crypto errors", "helper_rx_crypto_err", 1 if stats.get("helper_rx_crypto_err", 0) > 0 else 2)
            sline("Backpressure", "helper_rx_backpressure", 1 if stats.get("helper_rx_backpressure", 0) > 0 else 2)
            row += 1

            put(row, 2, f"Kern crypto: {stats.get('helper_rx_kern_crypto', 0):,}")
            put(row, 35, f"Pending: {stats.get('pending_count', 0)}")
            row += 2

            put(row, 1, "q=quit  r=reset  d=toggle drop_primary", DIM)

            stdscr.refresh()
            time.sleep(1.0 / REFRESH_HZ)

    curses.wrapper(draw)


# ---- Main -------------------------------------------------------------------

def main():
    if os.geteuid() != 0:
        print("Must run as root (needs debugfs access)")
        sys.exit(1)

    if "--status" in sys.argv or "-s" in sys.argv:
        print_status()
        return

    if "--watch" in sys.argv or "-w" in sys.argv:
        try:
            watch_loop()
        except KeyboardInterrupt:
            print("\nStopped.")
        return

    if not os.path.exists(STATS_PATH):
        print(f"Stats not found at {STATS_PATH}")
        print("Is the driver loaded with rtw_cooperative_rx=1?")
        sys.exit(1)

    try:
        curses_main()
    except ImportError:
        print("curses not available, using --watch mode")
        try:
            watch_loop()
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()
