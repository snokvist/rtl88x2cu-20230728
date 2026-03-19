# Cooperative RX Diversity Mode — Design Document
## RTL8822CU Vendor Driver (rtl88x2cu-20230728)

### Executive Feasibility Summary

**Verdict: FEASIBLE with careful implementation.**

The RTL8822CU vendor driver uses a cfg80211 + custom vendor MLME stack (NOT
mac80211). The RX path is entirely in-driver, giving us full control over
frame processing order. Two separate identical USB adapters each have their
own `dvobj_priv` and `_adapter` structures, but can be linked via a new
cross-device cooperative context.

The key challenge is that duplicate detection (`recv_decache`) and PN replay
checking (`recv_ucast_pn_decache`) occur early in the RX path (inside
`validate_recv_data_frame`), BEFORE the AMPDU reorder window. A helper
adapter's copy of a frame would be rejected as a duplicate or replay by the
primary's normal path. The solution is to inject helper frames into the
primary's RX path at a point where they bypass duplicate/PN checks that
already ran on the primary's copy, while still entering the reorder window
correctly.

**Optimal merge point:** After `recvbuf2recvframe()` parsing but BEFORE
`validate_recv_data_frame()`, inside `recv_func_prehandle()`. The helper
frame enters the primary adapter's `recv_indicatepkt_reorder()` directly,
where the reorder window provides natural duplicate suppression (same
sequence number = same slot = no double delivery).

### 1. Current Driver Architecture

#### 1.1 Stack Type
- **cfg80211 + vendor MLME** (NOT mac80211)
- Driver implements its own association state machine, AMPDU reorder,
  crypto, defragmentation
- Each USB device gets one `dvobj_priv` with up to CONFIG_IFACE_NUMBER
  virtual adapters

#### 1.2 RX Path (USB URB to network stack)

```
USB Hardware → usb_read_port_complete() [URB completion callback]
    → rx_skb_queue (enqueue SKB)
    → usb_recv_tasklet() [tasklet processing]
    → recvbuf2recvframe() [parse RX descriptor, extract PHY status]
        → rtl8821c_query_rx_desc() [24-byte descriptor parsing]
        → pre_recv_entry() [interface routing, PHY status query]
            → rtw_recv_entry() → recv_func()
                → recv_func_prehandle()
                    → validate_recv_frame()
                        → validate_recv_data_frame()
                            → recv_decache()           ← SEQ duplicate check
                            → recv_ucast_pn_decache()   ← PN replay check
                → recv_func_posthandle()
                    → decryptor()                       ← SW decryption
                    → recvframe_chk_defrag()            ← fragment reassembly
                    → portctrl()                        ← 802.1X port control
                    → recv_indicatepkt_reorder()        ← AMPDU reorder window
                        → check_indicate_seq()
                        → enqueue_reorder_recvframe()
                        → recv_indicatepkts_in_order()
                            → recv_process_mpdu()
                                → wlanhdr_to_ethhdr()
                                → rtw_recv_indicatepkt()
                                    → netif_rx() / napi_gro_receive()
```

#### 1.3 Key Structures

- `dvobj_priv`: Per-USB-device. Contains `padapters[]`, channel context,
  hardware mutexes. Each physical USB dongle has its own dvobj.
- `_adapter`: Per-interface. Contains `recvpriv`, `mlmepriv`, `stapriv`,
  `securitypriv`. Has `iface_id` and `dvobj` pointer.
- `sta_info`: Per-station. Contains `recvreorder_ctrl[16]` (per-TID reorder
  windows), `sta_recvpriv.rxcache.tid_rxseq[16]` (duplicate detection).
- `recv_reorder_ctrl`: Per-TID. `indicate_seq` (window start), `wsize_b`
  (window size, typically 64), `pending_recvframe_queue`.
- `rx_pkt_attrib`: Per-frame metadata. `seq_num`, `frag_num`, `priority`
  (TID), `encrypt`, `bdecrypted`, `bssid[]`, `ta[]`, `ra[]`, RSSI, etc.

#### 1.4 Duplicate Detection

`recv_decache()` (core/rtw_recv.c:863): Compares `(seq_num << 4 | frag_num)`
against per-STA per-TID `tid_rxseq[]`. Exact match = duplicate → drop.
Updates cache on accept.

#### 1.5 PN Replay Protection

`recv_ucast_pn_decache()` (core/rtw_recv.c:800): For AES/CCMP, extracts
48-bit PN from IV header. Checks `VALID_PN_CHK(pkt_pn, curr_pn)` which
requires strictly increasing PN. Updates cache on accept.

**Critical note:** The PN check currently has a commented-out `return _FAIL`
line (line 820), meaning PN replay for unicast is logged but NOT enforced in
the current codebase. This actually simplifies cooperative mode but is a
pre-existing security concern.

#### 1.6 AMPDU Reorder Window

`recv_indicatepkt_reorder()` (core/rtw_recv.c:3621):
- Window size typically 64 (`wsize_b`)
- `check_indicate_seq()`: Drops frames with `SN_LESS(seq_num, indicate_seq)`
- `enqueue_reorder_recvframe()`: Inserts in-order by sequence number into
  `pending_recvframe_queue`. If slot already occupied → frame is discarded
  (natural duplicate suppression).
- `recv_indicatepkts_in_order()`: Delivers consecutive frames starting at
  `indicate_seq`.
- Timer: 50ms timeout forces delivery of buffered frames.

**Key insight:** The reorder window provides natural duplicate suppression.
If we inject a helper frame into the reorder path, and the primary already
received that sequence number, it will already be in the queue or already
delivered. The `enqueue_reorder_recvframe()` function inserts by sequence
number and will find the slot occupied → discard the duplicate. This is our
primary dedup mechanism for AMPDU traffic.

#### 1.7 Hardware Decryption

The RTL8821C can decrypt in hardware. `bdecrypted` flag in `rx_pkt_attrib`
indicates whether HW already decrypted. If both adapters have the same keys
installed, both can HW-decrypt. If helper HW-decrypts, the frame arrives
already decrypted and can skip SW decryption on the primary path.

### 2. Cooperative Mode Design

#### 2.1 Architecture Overview

```
  USB Dongle A (Primary)          USB Dongle B (Helper)
  ┌──────────────────┐            ┌──────────────────┐
  │ dvobj_priv (A)   │            │ dvobj_priv (B)   │
  │   adapter (A)    │            │   adapter (B)    │
  │   normal RX path │            │   helper RX path │
  └────────┬─────────┘            └────────┬─────────┘
           │                               │
           │                    ┌──────────┘
           │                    │ cooperative_rx_submit()
           │                    ▼
           │         ┌─────────────────────┐
           │         │ Cooperative Merge    │
           │         │ - validate BSSID/TA  │
           │         │ - check seq/TID      │
           │         │ - inject into        │
           │         │   primary reorder    │
           │         └─────────┬───────────┘
           │                   │
           ▼                   ▼
     ┌─────────────────────────────────┐
     │ Primary adapter's reorder window│
     │ (natural duplicate suppression) │
     └─────────────┬───────────────────┘
                   │
                   ▼
          Network stack (one stream)
```

#### 2.2 Merge Point Selection

**Selected merge point:** `recv_func_posthandle()` on the primary adapter.
This runs the full decrypt → defrag → portctrl → reorder pipeline.

For the helper frame, we:
1. Parse the 802.11 header in `pre_recv_entry()` (BSSID, TA, RA, seq, TID)
2. Validate the frame belongs to the primary's active session
3. Fix up encryption fields from the primary's `security_priv` (monitor mode
   RX descriptor reports `encrypt=0` even for encrypted frames)
4. Allocate a recv_frame from the primary's pool, transfer the skb
5. Submit to `recv_func_posthandle()` which SW-decrypts using the primary's
   pairwise key, then feeds into the reorder window

**Why this point:**
- SW decryption works on encrypted networks (WPA2/WPA3) without HW CAM
- CCMP PN replay check provides natural dedup for in-window duplicates
- Reorder window catches out-of-window duplicates by sequence number
- For non-QoS/non-AMPDU traffic: explicit seq_num cache before submission
- HW decryption is not possible in monitor mode (RCR bypasses CAM lookup)

#### 2.3 Cooperative Context Structure

```c
/* Global cooperative RX group - spans across dvobj boundaries */
struct cooperative_rx_group {
    spinlock_t lock;
    enum coop_rx_state state;

    /* Primary adapter reference */
    _adapter *primary;
    struct dvobj_priv *primary_dvobj;

    /* Helper adapter(s) - extensible to N helpers */
    _adapter *helpers[COOP_MAX_HELPERS];
    struct dvobj_priv *helper_dvobjs[COOP_MAX_HELPERS];
    int num_helpers;

    /* Session binding */
    u8 bound_bssid[ETH_ALEN];   /* BSSID = AP MAC (TA) in infra BSS */
    u8 bound_channel;
    u8 bound_bw;

    /* Non-QoS dedup cache */
    struct coop_nonqos_seq_cache nonqos_cache;

    /* Statistics (nested struct, all atomic) */
    struct coop_rx_stats {
        atomic_t helper_rx_candidates;   /* frames considered */
        atomic_t helper_rx_accepted;     /* frames injected */
        atomic_t helper_rx_dup_dropped;  /* duplicates caught */
        atomic_t helper_rx_pool_full;    /* primary pool exhausted */
        atomic_t helper_rx_foreign;      /* wrong BSSID/TA */
        atomic_t helper_rx_crypto_err;   /* decrypt failures */
        atomic_t helper_rx_late;         /* too late for reorder */
        atomic_t helper_rx_no_sta;       /* sta_info not found */
        atomic_t fallback_events;        /* helper disappeared */
        atomic_t pair_events;            /* successful pairings */
        atomic_t unpair_events;          /* teardown events */
    } stats;
};
```

#### 2.4 Helper Frame Injection Flow

```
Helper USB URB completion
  → usb_recv_tasklet() [helper's own tasklet]
  → recvbuf2recvframe() [parse descriptor, get metadata]
  → cooperative_rx_submit() [NEW - instead of normal path]
      1. Check group->active
      2. Validate BSSID matches group->bound_bssid
      3. Validate TA matches expected AP MAC
      4. Extract seq_num, TID, encrypt status
      5. If encrypted and not HW-decrypted: attempt SW decrypt
         using primary's security context
      6. Acquire primary's reorder lock
      7. For QoS/AMPDU: inject into primary's
         recv_indicatepkt_reorder() via the per-STA
         recvreorder_ctrl[tid]
      8. For non-QoS: explicit seq check, then
         recv_process_mpdu() directly
      9. Update statistics
```

#### 2.5 Locking Model

- `cooperative_rx_group.lock` (spinlock): Protects group membership changes
  (pairing, unpairing, teardown)
- Primary's reorder queue lock: Already exists per-STA, reused for
  helper injection (same lock protects reorder window)
- RCU for group pointer access: Helper's hot path reads group pointer
  under RCU, teardown uses synchronize_rcu

#### 2.6 Lifecycle

```
States: DISABLED → IDLE → BINDING → ACTIVE → TEARDOWN → DISABLED

DISABLED: Module param off. No code paths touched.
IDLE:     Module param on, no helper paired.
BINDING:  Helper adapter detected, channel/BSSID sync in progress.
ACTIVE:   Helper contributing frames to primary's reorder window.
TEARDOWN: Helper unplugged or feature disabled, draining queues.
```

#### 2.7 Non-AMPDU Traffic Handling

For non-QoS frames that don't go through the reorder window:
- Maintain a small cooperative seq_num cache (last N accepted seq_nums)
- Helper frame accepted only if its seq_num is NOT in the cache
- This prevents duplicate delivery for non-AMPDU traffic

### 3. Safety Contract (Phase 1)

#### 3.1 Behavioral Contract

1. When `rtw_cooperative_rx=0` (default): **ZERO code path changes**.
   All cooperative code is behind runtime checks.
2. Primary adapter appears as ordinary wlan interface.
3. Helper adapter's netdev is DOWN and not independently usable while
   slaved.
4. Helper failure → graceful fallback to primary-only with log message.
5. No deadlock on USB disconnect/reset/suspend of either adapter.
6. AP/client mode compatibility preserved (first implementation: STA only).

#### 3.2 Non-Goals (First Implementation)

- No throughput bonding / bandwidth aggregation
- No AP mode cooperative RX (STA only first)
- No multi-channel operation
- No cross-BSSID roaming assistance
- No helper TX capability
- No automatic adapter discovery (manual pairing via sysfs/debugfs)

#### 3.3 Kill Switches

- Module parameter: `rtw_cooperative_rx` (0=disabled, 1=enabled, default=0)
- Runtime sysfs: `/sys/class/net/wlanX/coop_rx/enabled` (write 0 to disable)
- debugfs: `/sys/kernel/debug/rtw_coop_rx/` for stats and diagnostics
- Emergency: unplug helper adapter → instant fallback

### 4. Verification Gates

#### Gate 0 (Build)
- Compiles cleanly with cooperative_rx disabled
- Compiles cleanly with cooperative_rx enabled
- No new warnings in touched files

#### Gate 1 (Single-Adapter Regression)
With `rtw_cooperative_rx=0`:
- Association works
- DHCP works
- TCP iperf sustained
- UDP iperf works
- AMPDU negotiates
- Monitor mode works
- No kernel warnings

#### Gate 2 (Cooperative Idle)
With `rtw_cooperative_rx=1` but no helper paired:
- All Gate 1 tests pass unchanged
- No duplicate packets
- No throughput regression (< 2%)
- No kernel warnings

#### Gate 3 (Cooperative Active)
With helper paired and active:
- Single coherent RX stream to stack
- No duplicate delivery (tcpdump verification)
- Under primary attenuation: measurable packet rescue
- Stats counters incrementing correctly
- Helper unplug: clean fallback, no crash

### 5. Patch Sequence Plan

#### Patch 1: Infrastructure — cooperative_rx header and module param
- New file: `include/rtw_cooperative_rx.h`
- New file: `core/rtw_cooperative_rx.c`
- Module param in `os_dep/linux/os_intfs.c`
- Makefile addition
- **Gate:** Compiles, loads, param visible, no functional change

#### Patch 2: Cooperative group lifecycle
- Group create/destroy/bind/unbind
- sysfs interface for manual pairing
- Helper adapter state management
- **Gate:** Can pair/unpair adapters, no RX path changes

#### Patch 3: Helper RX interception
- Hook in helper's `pre_recv_entry()` to redirect to cooperative path
- Frame validation (BSSID, TA, session matching)
- **Gate:** Helper frames are intercepted and counted, not yet injected

#### Patch 4: Primary reorder injection
- Helper frames injected into primary's reorder window
- Duplicate suppression via reorder window
- Non-AMPDU dedup cache
- **Gate:** Helper frames visible in primary's stats, no double delivery

#### Patch 5: Statistics and debugfs
- All counters exposed
- Debug logging for troubleshooting
- **Gate:** Full observability

### 6. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Double packet delivery | Medium | High | Reorder window dedup + explicit seq check |
| Primary crash on helper unplug | Medium | Critical | RCU-protected group pointer, null checks |
| Reorder window corruption | Low | Critical | Same lock as normal path, careful injection |
| PN/replay confusion | Low | High | Helper frames use primary's PN state |
| Performance regression (disabled) | Low | Medium | All checks behind `if (unlikely(coop_enabled))` |
| Memory leak on teardown | Medium | Medium | Explicit cleanup, leak detection counters |
| Stale state after roam | Medium | High | Unbind on disconnect/roam, rebind after |
| Locking inversion deadlock | Low | Critical | Lock ordering: group lock → reorder lock |

### 7. Rollback Strategy

- Set `rtw_cooperative_rx=0` → immediate disable, zero code path impact
- Remove helper USB adapter → automatic fallback
- Revert patch series → all changes in isolated files + minimal hooks
- Each patch independently revertible in reverse order

### 8. Cross-Driver Compatibility

**Cooperative RX requires both adapters to use the same kernel module.**

Mixing different Realtek drivers (e.g. RTL8822CU primary + RTL8812AU helper)
is **not possible** for the following architectural reasons:

| Barrier | Detail |
|---------|--------|
| Module boundary | `rtw_coop_rx_group` is a per-module global. Two different `.ko` modules have separate instances with no visibility into each other. |
| Symbol isolation | `recv_func_posthandle()`, `rtw_alloc_recvframe()`, `rtw_get_stainfo()`, and all cooperative RX functions are internal module symbols (no `EXPORT_SYMBOL`). One module cannot call another's functions. |
| `netdev_ops` check | Sysfs pairing validates `ndev->netdev_ops == &rtw_netdev_ops`. Each `.ko` has its own `rtw_netdev_ops` at a different address — cross-module pairing is rejected. |
| Struct layout | `_adapter`, `recv_frame`, `rx_pkt_attrib`, `sta_info` sizes depend on CONFIG flags and HAL-layer structs that differ between chip families. Sharing frames across modules would cause memory corruption. |
| HAL layer | RX descriptor format, `query_rx_desc()`, PHY status parsing, and crypto HW registers are chip-specific. A frame parsed by one chip's HAL cannot be processed by another's. |
| recv_frame pool | Each adapter has its own 256-frame pool with driver-specific allocation. Cross-module pool sharing is not possible. |

**What DOES work:**

- Two identical chipset adapters (e.g. two RTL8822CU) loaded by the same
  driver module — this is the tested and supported configuration.
- The code can be **ported independently** to other Realtek vendor drivers
  (e.g. rtl8812au, rtl8812eu from [libc0607](https://github.com/libc0607)).
  After porting, two RTL8812AU adapters could cooperate with each other, and
  two RTL8812EU adapters with each other, but not across driver boundaries.

**Hypothetical cross-driver support** would require:
1. A shared kernel module (`rtw-coop-base.ko`) exporting the cooperative
   group, frame submission, and key query functions
2. Stable ABI contracts with versioned symbols (`EXPORT_SYMBOL_GPL`)
3. Unified struct definitions in shared headers
4. Per-driver translation layers to convert chip-specific `recv_frame` data
   into a common cooperative frame format
5. Cross-module device enumeration (similar to kernel bridge/bonding)

This is a significant engineering effort and is not planned.

### 9. Porting Guide

**Copy these files unchanged** into the target driver:

- `core/rtw_cooperative_rx.c` — all cooperative RX logic, header parsing,
  SW decrypt fixup, PN pre-check, sysfs/debugfs interfaces
- `include/rtw_cooperative_rx.h` — structs, enums, inline helpers

**Add these hooks** (each is 1-4 lines):

| Hook | File | What to add |
|---|---|---|
| RX intercept | `core/rtw_recv.c` in `pre_recv_entry()` | `if (rtw_coop_rx_pre_recv_entry(...) == RTW_RX_HANDLED) goto exit;` before monitor mode check |
| CSA follow | `core/rtw_cmd.c` in `rtw_dfs_ch_switch_hdl()` | `rtw_coop_rx_notify_channel_switch(pri_adapter);` after `set_channel_bwmode` + `#include <rtw_cooperative_rx.h>` |
| Channel set | `core/rtw_mlme_ext.c` in `set_ch_hdl()` | `rtw_coop_rx_notify_channel_switch(padapter);` after channel update |
| Init/deinit | `os_dep/linux/usb_intf.c` | `rtw_coop_rx_init()` before `usb_register()`, `rtw_coop_rx_deinit()` in exit + error path |
| Export ops | `os_dep/linux/os_intfs.c` | Remove `static` from `rtw_netdev_ops` |
| Header decls | `include/rtw_recv.h` | Add `recv_func_posthandle()` extern + PN macros |
| Makefile | `Makefile` | Add `core/rtw_cooperative_rx.o` to `rtk_core` |

**Optional noise reduction** (not required for functionality):

| File | Change |
|---|---|
| `core/rtw_swcrypto.c` | `RTW_INFO` → `RTW_DBG` for CCMP/GCMP decrypt failure |
| `os_dep/linux/ioctl_cfg80211.c` | `RTW_INFO` → `RTW_DBG` for `get_txpower`/`get_channel` |

All Realtek vendor drivers from the same generation (libc0607 repos:
rtl88x2cu, rtl88x2eu, rtl8812au, rtl8812eu) share identical function
signatures for `pre_recv_entry`, `recv_func_posthandle`, `set_channel_bwmode`,
`rtw_get_stainfo`, `GET_ENCRY_ALGO`, `SET_ICE_IV_LEN`, and `rtw_iv_to_pn`.
The hooks are pure insertions — no existing code needs modification beyond
removing `static` from `rtw_netdev_ops`.
