# Cooperative RX Testing Notes — 2026-03-19

## What WORKS

1. **Driver compiles cleanly** with cooperative RX code — no warnings
2. **Module loads** with `rtw_cooperative_rx=1` — parameter visible at `/sys/module/88x2cu/parameters/`
3. **sysfs** `coop_rx/` directory appears on all interfaces with 6 attributes
4. **debugfs** at `/sys/kernel/debug/rtw_coop_rx/stats` works correctly
5. **Pairing** primary/helper via sysfs works — state transitions correctly
6. **Binding** session to active BSS works — captures BSSID, channel, BW
7. **Single adapter connects fine** to `waybeam-03` (WPA2-PSK CCMP, ch157 5GHz)
8. **DHCP works** — gets 10.6.0.x from AP, pings 10.6.0.1 fine
9. **USB unbind/rebind** works as software unplug/replug to avoid auth interference
10. **Safe startup script** — unload driver BEFORE USB unbind prevents PC hangs
11. **coop-rx-start.sh** runs end-to-end with continuous helper RX (~1000 frames/sec)
12. **coop-rx-monitor.py** ncurses dashboard parses and displays all stats correctly
13. **Helper in monitor mode** — driver-internal monitor mode via
    `rtw_coop_rx_enable_helper_monitor()` works correctly, cfg80211 reports
    `monitor` type, helper receives continuously on bound channel
14. **Recv frame pool isolation** — helper frames are transferred to primary-
    allocated recv_frames before injection, preventing pool drain

## What WAS FIXED (2026-03-19)

### Fix 1: Monitor mode via driver internal APIs
- **Problem**: `iw dev set type monitor` when interface is down only updates
  cfg80211 type, NOT the driver's internal `WIFI_MONITOR_STATE`. RCR stays
  closed, RX filter maps stay restrictive.
- **Fix**: Added `rtw_coop_rx_enable_helper_monitor()` that calls
  `rtw_set_802_11_infrastructure_mode(Ndis802_11Monitor)` +
  `rtw_setopmode_cmd()` + `set_channel_bwmode()` + sets
  `wdev->iftype = NL80211_IFTYPE_MONITOR`. Called automatically from
  `rtw_coop_rx_bind_session()`.

### Fix 2: Recv frame pool drain
- **Problem**: `rtw_coop_rx_submit_helper_frame()` reassigned
  `precvframe->u.hdr.adapter = primary`. When freed, frames returned to
  primary's pool instead of helper's. Helper's 256-frame pool drained
  permanently, causing `rtw_alloc_recvframe() failed! RX Drop!`.
- **Fix**: Allocate a new recv_frame from primary's pool, transfer the skb,
  free the helper's frame back to its own pool immediately. All validation
  runs on the original frame BEFORE allocation, so on rejection the caller
  still owns a valid frame.

### Fix 3: NetworkManager killing helper interface
- **Problem**: Script restarted NM AFTER pair/bind. NM brought helper
  interface down during its 2s startup, killing USB bulk-in URBs. The
  driver's `_netdev_open` only calls `rtw_intf_start` (which submits URBs)
  when `!rtw_is_hw_init_completed` — on reopen the HW is already init'd,
  so URBs were never resubmitted.
- **Fix**: Moved NM restart + `nmcli device set managed no` to BEFORE
  pair/bind phase. NM is settled before monitor mode is configured.

## Statistics Counters Reference

All counters are atomic and visible via:
- **debugfs**: `cat /sys/kernel/debug/rtw_coop_rx/stats`
- **sysfs**: `cat /sys/class/net/<primary>/coop_rx/coop_rx_stats`

### Frame flow counters

| Counter | Meaning |
|---------|---------|
| `helper_rx_candidates` | Total data frames received by the helper that entered the cooperative merge path. This is the top of the funnel — every 802.11 data frame from the helper that passes initial BSSID/TA/RA validation. |
| `helper_rx_accepted` | Frames successfully injected into the primary's RX path (reorder window for QoS, or directly delivered for non-QoS). These frames contribute to the primary's network stack. A high accepted/candidates ratio means the helper is effectively filling gaps. |
| `helper_rx_dup_dropped` | Frames rejected because the primary already received them. For QoS/AMPDU traffic, this means the reorder window slot was already occupied. For non-QoS, the sequence number was in the dedup cache. Also incremented when the primary's recv_frame pool is exhausted. |
| `helper_rx_foreign` | Frames rejected because they don't belong to the bound session. Either the BSSID doesn't match the associated AP, the TA (transmitter address) doesn't match, or the RA (receiver address) doesn't match the primary's MAC and isn't broadcast/multicast. High counts here indicate RF interference from other networks. |
| `helper_rx_late` | QoS/AMPDU frames where the sequence number is behind the primary's reorder window start (`indicate_seq`). The primary has already moved past this point in the stream — the helper's copy arrived too late to be useful. A high late count relative to candidates suggests the helper has higher latency than the primary (longer USB path, slower processing). |
| `helper_rx_crypto_err` | Frames with CRC/ICV errors, or encrypted frames that weren't decrypted by the helper's hardware. In the current implementation, SW decryption of helper frames is not supported — if the helper's HW didn't decrypt the frame, it's dropped. |
| `helper_rx_no_sta` | Frames where the transmitter address couldn't be found in the primary's station table (`sta_info`). This shouldn't happen during normal operation — if it does, the primary may have disassociated or the AP changed its MAC. |

### System event counters

| Counter | Meaning |
|---------|---------|
| `pair_events` | Number of times a helper adapter was paired to the group (via sysfs `coop_rx_pair`). |
| `unpair_events` | Number of times a helper was removed (sysfs unpair, USB disconnect, or driver unload). |
| `fallback_events` | Number of times the system fell back to primary-only mode. Happens when the last helper is removed or the primary adapter is disconnected. |

### Interpreting the numbers

- **Healthy operation**: `accepted` is close to `candidates`, `dup_dropped`
  is moderate (shows overlap — both adapters see the same frames),
  `late` is low.
- **Helper adding value**: `accepted` > 0 and some frames in `accepted`
  weren't received by the primary (visible as throughput improvement or
  gap filling during fading).
- **Helper not useful**: `dup_dropped` ≈ `candidates` — primary already has
  everything. Helper is redundant but not harmful.
- **Latency issue**: `late` >> `accepted` — helper consistently arrives
  after the primary's reorder window has moved on.

### Group state values

| State | Value | Meaning |
|-------|-------|---------|
| `DISABLED` | 0 | Module parameter off or group destroyed |
| `IDLE` | 1 | Cooperative RX enabled, primary set, waiting for bind |
| `BINDING` | 2 | Helpers paired but session not yet bound to a BSS |
| `ACTIVE` | 3 | Fully operational — helper frames being merged |
| `TEARDOWN` | 4 | Shutting down (transient during adapter removal) |

## Architecture

```
Primary adapter (managed mode, associated to AP)
  └─ Normal RX path → network stack
  └─ Cooperative RX: helper frames injected into reorder window

Helper adapter (monitor mode via driver internal API)
  └─ WIFI_MONITOR_STATE set by rtw_coop_rx_enable_helper_monitor()
  └─ cfg80211 wdev iftype set to NL80211_IFTYPE_MONITOR
  └─ Channel parked via set_channel_bwmode()
  └─ RCR set to promiscuous (BIT_AAP), filter maps 0xFFFF
  └─ recv_func() hook intercepts data frames before recv_frame_monitor()
  └─ Validates: BSSID match, TA match, RA match
  └─ Allocates primary recv_frame, transfers skb, frees helper frame
  └─ Injects primary frame into recv_indicatepkt_reorder()
```

## Scripts

| Script | Purpose |
|--------|---------|
| `scripts/coop-rx-start.sh` | Full setup: unload, USB unbind, load, connect, rebind, NM restart, pair, bind |
| `scripts/coop-rx-stop.sh` | Tear down: kill wpa_supplicant, print final stats |
| `scripts/coop-rx-monitor.py` | Live ncurses dashboard: stats, rates, interface status |

## Hardware

- Two RTL8822CU USB NICs (`0bda:c812`)
- On separate USB controllers (bus 1 and bus 7)
- AP: `waybeam-03` BSSID `98:03:cf:cf:a4:28`, ch157 (5785 MHz), WPA2-PSK CCMP
- Host: Linux 6.14.0-37-generic, NL regulatory domain

## PC Hang History

- Two hangs during testing, both during USB unbind while driver was loaded + doing ACS I/O
- **Fixed**: Script now unloads driver BEFORE USB unbind — no hangs since
