# Cooperative RX Performance Optimization Plan

## Status: Planned

## Context

With kernel crypto (`CONFIG_COOP_RX_KERNEL_CRYPTO`) enabled, per-frame
AES-CCMP decrypt dropped from ~80-100 µs (pure C Rijndael) to ~2-5 µs
(ARMv8-CE) on Cortex-A55. CPU load improved substantially, but remains
too high when running 2 helper adapters at video stream rates.

This document covers the three remaining performance blockers after kernel
crypto. These are complementary to the CAM mirroring investigation
(see `coop_rx_cam_mirroring.md`) — if CAM mirroring works, blocker #1
is eliminated and blocker #2 becomes less critical.

## Priority Order

1. **CAM mirroring** (separate doc) — eliminates crypto entirely. Try first.
2. **Blocker #2: Drain tasklet scaling** — immediate impact on multi-helper.
3. **Blocker #1: aead_request pre-allocation** — quick win, low risk.
4. **Blocker #3: SKB alloc/memcpy** — largest effort, moderate payoff.

---

## Blocker #1: Per-frame `aead_request_alloc/free` (GFP_ATOMIC)

### Location

`core/rtw_cooperative_rx.c:261,283` — inside `coop_rx_kernel_decrypt()`

### Problem

Every helper frame that needs kernel crypto decryption does:

```c
req = aead_request_alloc(tfm, GFP_ATOMIC);  // line 261 — slab alloc
// ... setup scatterlist, AAD, decrypt ...
aead_request_free(req);                       // line 283 — slab free
```

Each cycle costs ~300-700 ns on Cortex-A55 (slab fast path + cache
pressure). With 2 helpers at 2000 fps each = 4000 alloc/free per second:
~1.2-2.8 ms/s of pure allocation overhead.

Worse: `GFP_ATOMIC` can fail under memory pressure in softirq context.
Failure triggers fallback to the pure C SW decrypt path (80-100 µs).

### Fix

Pre-allocate one `aead_request` per group during `coop_rx_crypto_init()`
(process context, `GFP_KERNEL` — can sleep, never fails under normal
memory conditions). Store in the `cooperative_rx_group` struct:

```c
struct cooperative_rx_group {
    ...
#ifdef CONFIG_COOP_RX_KERNEL_CRYPTO
    struct crypto_aead *tfm_ccm;
    struct crypto_aead *tfm_ccm_256;
    struct crypto_aead *tfm_gcm;
    u8 cached_key[32];
    u8 cached_key_len;
    /* NEW: pre-allocated request (sized for largest transform) */
    struct aead_request *prealloc_req;
    struct scatterlist prealloc_sg[2];
    u8 prealloc_aad[32];
#endif
    ...
};
```

In `coop_rx_crypto_init()`:
```c
/* Allocate for the largest transform's reqsize */
size_t max_reqsize = 0;
if (grp->tfm_ccm)
    max_reqsize = max(max_reqsize, crypto_aead_reqsize(grp->tfm_ccm));
if (grp->tfm_gcm)
    max_reqsize = max(max_reqsize, crypto_aead_reqsize(grp->tfm_gcm));

grp->prealloc_req = kmalloc(sizeof(struct aead_request) + max_reqsize,
                             GFP_KERNEL);
```

In `coop_rx_kernel_decrypt()`:
```c
/* Replace aead_request_alloc/free with: */
req = grp->prealloc_req;
aead_request_set_tfm(req, tfm);
aead_request_set_callback(req, 0, NULL, NULL);
// ... rest unchanged ...
/* No aead_request_free() — reused next frame */
```

**Safety:** The drain tasklet is serialized (single instance, never
concurrent with itself). The pre-allocated request is only accessed
from the drain tasklet, so no locking needed.

### Effort: Small (~30 lines changed)

### Risk: Very low

### Expected savings: ~1-3 ms/s at 4000 fps, eliminates GFP_ATOMIC failures

---

## Blocker #2: Single Drain Tasklet — Doesn't Scale with Helpers

### Location

`core/rtw_cooperative_rx.c:1382-1596` — `_coop_rx_drain_tasklet()`
`include/rtw_cooperative_rx.h:132-133` — `COOP_PENDING_MAX=48, COOP_BATCH_SIZE=64`

### Problem

All helper frames from ALL helpers funnel through one `coop_rx_tasklet`.
Tasklets are:
- **Serialized** — only one instance runs at a time
- **CPU-pinned** — runs on the CPU that called `tasklet_schedule()`
- **Budget-limited** — `COOP_BATCH_SIZE=64` frames per invocation

With 2 helpers at video rates:

| Metric | 1 helper | 2 helpers |
|--------|----------|-----------|
| Aggregate frame rate | ~2000 fps | ~4000 fps |
| Drain tasklet time | ~20 ms/s | ~40 ms/s |
| Queue fill rate during AMPDU burst | ~20 frames/ms | ~40 frames/ms |
| Time to fill COOP_PENDING_MAX=48 | ~2.4 ms | **~1.2 ms** |

The 48-frame pending queue overflows during any AMPDU burst longer than
~1.2 ms when the tasklet is preempted. This triggers `helper_rx_backpressure`
drops — frames the helper received but couldn't process.

### Fix: Three escalating options

#### Option A: Scale COOP_PENDING_MAX with helpers (quick)

```c
#define COOP_PENDING_BASE   48
#define COOP_PENDING_MAX    (COOP_PENDING_BASE * (1 + grp->num_helpers))
```

With 2 helpers: 48 * 3 = 144 frames. Absorbs ~3.6 ms of burst without
drops. Costs ~144 * sizeof(union recv_frame) from primary's pool — still
well within the 256-frame NR_RECVFRAME budget (leaves 112 for primary).

**Effort:** 5 lines. **Risk:** Very low.

#### Option B: Convert to workqueue (medium)

Replace the tasklet with a `kthread_worker` or `alloc_workqueue()`:

```c
/* In cooperative_rx_group: */
struct workqueue_struct *drain_wq;
struct work_struct drain_work;
```

Benefits:
- `queue_work()` can run on any CPU (vs tasklet pinned to scheduling CPU)
- Can use `WQ_UNBOUND` for automatic CPU load balancing
- No `COOP_BATCH_SIZE` limit — processes until queue is empty
- Can be made per-helper (one work item per helper) for true parallelism

Drawback: work items have slightly higher scheduling latency than
tasklets (~5-10 µs vs ~1-2 µs). Acceptable at video frame rates.

**Effort:** ~60 lines. **Risk:** Low.

#### Option C: Inline processing in helper's recv_tasklet (best)

**Why:** The original motivation for deferred processing was that
SW decrypt at 80-100 µs/frame caused the helper's recv_tasklet to
hold softirq too long, starving the primary's USB URB completions
and causing 60-86% packet loss. With kernel crypto at 2-5 µs/frame,
this is no longer a concern.

Instead of:
```
helper USB tasklet → submit_helper_frame() → enqueue pending → schedule drain → drain_tasklet → decrypt → reorder
```

Do:
```
helper USB tasklet → submit_helper_frame() → decrypt → inject into primary's reorder window
```

This eliminates the pending queue entirely. Each helper's USB tasklet
naturally runs on the CPU handling that adapter's USB interrupts,
giving implicit multi-CPU distribution.

The key concern is that injecting into the primary's reorder window
from the helper's tasklet could race with the primary's own recv path.
Mitigation: the reorder window is already protected by its per-TID lock.

**Effort:** ~100 lines (restructure submit + drain into one path).
**Risk:** Medium — needs careful testing for reorder window races.

### Recommendation

Implement A first (5 minutes, eliminates drops). Then B or C based on
whether CAM mirroring succeeds:
- If CAM mirroring works: per-frame cost is ~3-5 µs (no crypto), so
  Option A alone may suffice. Monitor `helper_rx_backpressure`.
- If CAM mirroring fails: implement C (inline) to distribute crypto
  across CPUs.

---

## Blocker #3: Per-frame SKB Alloc + Memcpy in USB Deaggregation

### Location

`os_dep/linux/recv_linux.c:89-99` — `rtw_os_alloc_recvframe()`
`hal/rtl8822c/usb/rtl8822cu_recv.c:72-170` — `recvbuf2recvframe()`

### Problem

Every frame from USB (primary AND all helpers) gets:

```c
pkt_copy = rtw_skb_alloc(alloc_sz);                    // ~300-500 ns
_rtw_memcpy(pkt_copy->data, pdata, skb_len);           // ~500-1500 ns
```

This is required because USB aggregation packs multiple frames into one
URB transfer buffer, and the buffer must be resubmitted immediately.
Each frame must be copied out before the URB is recycled.

With 3 adapters (1 primary + 2 helpers): 3x the allocation and copy
pressure. At 4000 fps aggregate with ~1400 byte frames:
- SKB alloc: 4000 * ~400 ns = ~1.6 ms/s
- memcpy: 4000 * ~1000 ns = ~4.0 ms/s
- **Total: ~5.6 ms/s** — plus L1/L2 cache pollution

### Fix options

#### Option A: SKB recycling pool

Maintain a per-adapter ring of pre-allocated SKBs. Instead of
`rtw_skb_alloc()` + eventual `kfree_skb()`, recycle freed SKBs back
into the pool:

```c
struct skb_pool {
    struct sk_buff *ring[POOL_SIZE];
    unsigned int head, tail;
};

static inline struct sk_buff *skb_pool_get(struct skb_pool *pool, unsigned int size)
{
    if (pool->head != pool->tail) {
        struct sk_buff *skb = pool->ring[pool->tail++];
        pool->tail &= (POOL_SIZE - 1);
        skb_reset_tail_pointer(skb);
        skb->len = 0;
        return skb;
    }
    return rtw_skb_alloc(size);  /* fallback */
}
```

Pre-warmed SKBs stay in CPU cache, eliminating slab allocator overhead.

**Effort:** ~80 lines. **Risk:** Low-Medium (must handle varying SKB sizes).

#### Option B: Zero-copy for single-frame USB transfers

When USB aggregation count is 1 (common at low-moderate rates), the
entire USB SKB contains just one frame. Instead of allocating a new SKB
and copying, reuse the USB buffer directly:

```c
if (pkt_cnt == 1 && pskb) {
    /* Single frame — transfer USB SKB directly */
    skb_pull(pskb, RXDESC_SIZE + pattrib->drvinfo_sz + pattrib->shift_sz);
    skb_trim(pskb, pattrib->pkt_len);
    precvframe->u.hdr.pkt = pskb;
    /* Allocate a new SKB for the next USB transfer */
    ...
}
```

This eliminates the memcpy entirely for non-aggregated transfers.

**Effort:** ~40 lines. **Risk:** Medium (USB buffer lifecycle management).

### Recommendation

These are lower priority than blockers #1 and #2. The combined ~5.6 ms/s
is noticeable but not the dominant cost. Address after CAM mirroring
and tasklet scaling are resolved.

---

## Implementation Order

```
Phase 1 (immediate):
  ├─ CAM mirroring probe test (coop_rx_cam_mirroring.md)
  └─ COOP_PENDING_MAX scaling (Blocker #2, Option A)

Phase 2 (based on Phase 1 results):
  ├─ If CAM works: full CAM mirroring implementation
  │   └─ Monitor helper_rx_backpressure — if still high, add Option B (workqueue)
  └─ If CAM fails: aead_request pre-alloc (#1) + inline processing (#2C)

Phase 3 (polish):
  └─ SKB recycling (#3A) if CPU is still above target
```

## Measurement Plan

Before and after each change, on Radxa Zero 3 (4x Cortex-A55 @ 1.4 GHz):

```bash
# CPU per core
mpstat -P ALL 1 10

# Coop RX stats (frame rates, drops, crypto)
cat /sys/class/net/wlan0/coop_rx/stats

# Key counters to watch:
#   helper_rx_backpressure  — pending queue overflow (blocker #2)
#   helper_rx_kern_crypto   — kernel crypto invocations (blocker #1)
#   helper_rx_accepted      — total helper frames delivered
#   helper_rx_dup_dropped   — dedup efficiency
```

Target: 2 helpers active, sustained video stream, total coop RX CPU
overhead < 5% of one A55 core.

## Code Locations Reference

| What | File | Lines |
|------|------|-------|
| aead_request_alloc | core/rtw_cooperative_rx.c | 261 |
| aead_request_free | core/rtw_cooperative_rx.c | 283 |
| coop_rx_crypto_init | core/rtw_cooperative_rx.c | 92-128 |
| Drain tasklet | core/rtw_cooperative_rx.c | 1382-1596 |
| COOP_PENDING_MAX | include/rtw_cooperative_rx.h | 132 |
| COOP_BATCH_SIZE | include/rtw_cooperative_rx.h | 133 |
| submit_helper_frame | core/rtw_cooperative_rx.c | 1061-1355 |
| SKB alloc + memcpy | os_dep/linux/recv_linux.c | 89-99 |
| recvbuf2recvframe | hal/rtl8822c/usb/rtl8822cu_recv.c | 72-170 |
| USB agg count | hal/rtl8822c/usb/rtl8822cu_recv.c | 98 |
| recv_frame pool | core/rtw_recv.c | 109-135 (NR_RECVFRAME=256) |
