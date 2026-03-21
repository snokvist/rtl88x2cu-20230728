# Cooperative RX Testing Notes — 2026-03-21

## What WORKS

1. **Driver compiles cleanly** with cooperative RX code — no warnings
2. **Module loads** with `rtw_cooperative_rx=1` — parameter visible at `/sys/module/88x2cu/parameters/`
3. **sysfs** `coop_rx/` directory appears on all interfaces with 10 attributes
   (`enabled`, `role`, `stats`, `info`, `pair`, `unpair`, `auto_pair`, `bind`,
   `reset_stats`, `drop_primary`)
4. **debugfs** at `/sys/kernel/debug/rtw_coop_rx/stats` works correctly
5. **Pairing** primary/helper via sysfs works — state transitions correctly
6. **Auto-pair** via `coop_rx_auto_pair` discovers helpers automatically
7. **Binding** session to active BSS works — captures BSSID, channel, BW
8. **STA mode** — 10/10 test scenarios pass (baseline, diversity, helper-only,
   primary-only, unpair mid-stream, re-pair, stats consistency, CSA channel
   switch forward+return, stats reset)
9. **AP mode** — 6/6 test scenarios pass (binding with own MAC as BSSID,
   remote STA association, uplink cooperative RX, helper-only AP mode,
   stats consistency, bidirectional traffic). Per-STA crypto key lookup
   verified with zero crypto/no_sta errors.
10. **AP CSA** — `hostapd_cli chan_switch` followed by helper via
    `createbss_hdl` hook. Stress-tested with 8 consecutive CSA cycles
    (149↔157, 5s apart), all passed — state ACTIVE throughout, zero
    crypto errors, helper channel matches primary after every switch.
11. **SW decryption** — helper frames on WPA2 are SW-decrypted using primary's
    pairwise key via `recv_func_posthandle()`. CCMP PN replay check provides
    natural duplicate suppression.
12. **Helper monitor mode** — `enable_helper_monitor()` brings interface UP
    (if DOWN), sets radiotap type, configures WIFI_MONITOR_STATE, disables
    ACS, blocks cfg80211 scans. Helper stays locked on bound channel.
13. **STA CSA** — helpers follow primary to new channel automatically via
    `set_ch_hdl` and `rtw_dfs_ch_switch_hdl` hooks
14. **Recv frame pool isolation** — helper frames transferred to primary pool
15. **Per-STA non-QoS dedup** — cache entries keyed on (seq, TA), prevents
    false dups from different STAs in AP mode
16. **RSSI observability** — `helper_rx_rssi_better/worse` counters track
    signal quality comparison between helper and primary
17. **Sysfs input validation** — non-RTW interface names rejected
18. **Namespace-aware** — sysfs pair/unpair use `rtw_get_same_net_ndev_by_name()`

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

### Fix 4: Encrypted helper frames corrupting primary RX
- **Problem**: Helper in monitor mode receives encrypted frames. RX descriptor
  reports `encrypt=0` (HW CAM lookup bypassed in monitor mode), so the
  encryption check passed, and raw encrypted data was injected into the
  primary's reorder window. This displaced correctly-decrypted frames,
  breaking connectivity (100% ping loss while cooperative RX was active).
- **Root cause**: Monitor mode sets RCR to `BIT_AAP` (Accept All Packets),
  which disables HW CAM lookup. Even with the primary's key written into
  the helper's CAM, the HW never attempts decryption.
- **Fix**: Detect encryption from the 802.11 Protected Frame bit
  (`pattrib->privacy`). When `privacy=1` and `encrypt=0`, populate
  `encrypt`, `iv_len`, `icv_len`, `hdrlen` from the primary's security
  context (`GET_ENCRY_ALGO`, `SET_ICE_IV_LEN`). Changed injection point
  from `recv_indicatepkt_reorder()` to `recv_func_posthandle()`, which
  runs the SW decryptor (`rtw_aes_decrypt`) using the primary's pairwise
  key. CCMP PN replay check provides natural duplicate suppression.

### Fix 5: Dead code in recv_func()
- **Problem**: `recv_func()` had a cooperative RX hook (lines 4344-4400)
  that was unreachable — `pre_recv_entry()` already intercepts all helper
  data frames before `recv_func()` is called.
- **Fix**: Removed the entire dead block.

### Fix 6: Missing deinit on usb_register failure
- **Problem**: `rtw_coop_rx_init()` was called before `usb_register()`, but
  `rtw_coop_rx_deinit()` was missing from the error path when
  `usb_register()` failed.
- **Fix**: Added `rtw_coop_rx_deinit()` to the error cleanup path.

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
| `helper_rx_dup_dropped` | Frames rejected because the primary already received them. For encrypted frames, the CCMP PN replay check catches duplicates (PN already consumed by the primary's copy). For non-QoS unencrypted traffic, the sequence number dedup cache catches them. Also includes frames rejected by `recv_func_posthandle()` (decrypt failure, defrag error). |
| `helper_rx_pool_full` | Primary adapter's recv_frame pool was exhausted — no free frames available to receive the helper's contribution. Indicates memory pressure; if persistent, the primary's pool size may need increasing. |
| `helper_rx_foreign` | Frames rejected because they don't belong to the bound session. Either the BSSID doesn't match the associated AP, the TA (transmitter address) doesn't match, or the RA (receiver address) doesn't match the primary's MAC and isn't broadcast/multicast. High counts here indicate RF interference from other networks. |
| `helper_rx_late` | QoS/AMPDU frames where the sequence number is behind the primary's reorder window start (`indicate_seq`). The primary has already moved past this point in the stream — the helper's copy arrived too late to be useful. A high late count relative to candidates suggests the helper has higher latency than the primary (longer USB path, slower processing). |
| `helper_rx_crypto_err` | Frames with CRC/ICV errors, or frames where the encryption algorithm couldn't be determined (primary has no keys). Should be 0 during normal WPA2 operation — SW decryption handles encrypted helper frames via `recv_func_posthandle()`. Non-zero indicates the primary isn't associated or has no pairwise key. |
| `helper_rx_no_sta` | Frames where the transmitter address couldn't be found in the primary's station table (`sta_info`). This shouldn't happen during normal operation — if it does, the primary may have disassociated or the AP changed its MAC. |
| `helper_rx_deferred` | Frames enqueued to the pending queue for processing by the drain tasklet. Should track close to `accepted` — the difference is frames dropped by the drain tasklet's dedup check. |
| `helper_rx_backpressure` | Frames dropped because the pending queue exceeded `COOP_PENDING_MAX` (128). Indicates the drain tasklet can't keep up. Should be 0 under normal load. |
| `helper_rx_rssi_better` | Accepted helper frames where the helper's `recv_signal_power` exceeded the primary's running RSSI average. Pure observability — does not affect frame acceptance (first-wins). |
| `helper_rx_rssi_worse` | Accepted helper frames where the helper's RSSI was equal to or below the primary's average. |

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

## Multiple Helpers

The cooperative RX code supports up to `COOP_MAX_HELPERS` (4) helpers
simultaneously. All pairing, binding, channel switching, and teardown
paths iterate over the full helper array — no code changes needed.

To add additional helpers after the first is paired:

```bash
# Pair a second helper (must be same chipset/driver)
echo "wlan2" > /sys/class/net/<primary>/coop_rx/coop_rx_pair

# If session is already bound, re-bind to set up monitor mode on the new helper
echo 1 > /sys/class/net/<primary>/coop_rx/coop_rx_bind
```

The `coop-rx-start.sh` script handles one helper. For multiple helpers,
modify it to unbind/rebind additional USB devices in the same pattern.

Each helper independently contributes frames — the primary's reorder
window and PN replay check handle dedup across all helpers naturally.
Stats are aggregated across all helpers (not per-helper).

## Architecture

```
Primary adapter (STA or AP mode)
  └─ Normal RX path → network stack
  └─ Cooperative RX: helper frames SW-decrypted and injected via
     recv_func_posthandle() (decrypt → defrag → portctrl → reorder)
  └─ STA mode: captures AP→STA downlink (to_fr_ds=2)
  └─ AP mode: captures STA→AP uplink (to_fr_ds=1), per-STA key lookup

Helper adapter (monitor mode via driver internal API)
  └─ enable_helper_monitor(): dev_open (if DOWN) → radiotap type →
     WIFI_MONITOR_STATE → ACS disable → scan block → set_channel_bwmode
  └─ pre_recv_entry() hook intercepts data frames
  └─ Validates: direction, BSSID, TA/RA (mode-aware)
  └─ Fixes up encrypt/iv_len/icv_len from primary's security context
  └─ Allocates primary recv_frame, transfers skb, frees helper frame
  └─ Enqueues to pending_queue → drain tasklet processes deferred
  └─ CCMP PN replay check provides natural duplicate suppression
```

## Scripts

| Script | Purpose |
|--------|---------|
| `scripts/coop-rx-start.sh` | STA mode setup: unload, USB unbind, load, connect, rebind, NM restart, pair, bind |
| `scripts/coop-rx-stop.sh` | Tear down: kill wpa_supplicant, print final stats |
| `scripts/coop-rx-test.sh` | STA mode test suite: 10 scenarios including CSA channel switch |
| `scripts/coop-rx-monitor.py` | Live ncurses dashboard: stats, rates, AP/STA mode display |
| `scripts/coop-rx-ap-test-start.sh` | AP mode setup: hostapd on primary, remote STA via SSH |
| `scripts/coop-rx-ap-test-verify.sh` | AP mode test suite: 6 scenarios |
| `scripts/coop-rx-ap-test-stop.sh` | AP mode teardown: stop hostapd, restore remote AP |

## Hardware

- Two RTL8822CU USB NICs (`0bda:c812`)
- On separate USB controllers (bus 1 and bus 7)
- AP: `waybeam-03` BSSID `98:03:cf:cf:a4:28`, ch157 (5785 MHz), WPA2-PSK CCMP
- Host: Linux 6.14.0-37-generic, NL regulatory domain

## PC Hang History

- Two hangs during testing, both during USB unbind while driver was loaded + doing ACS I/O
- **Fixed**: Script now unloads driver BEFORE USB unbind — no hangs since
