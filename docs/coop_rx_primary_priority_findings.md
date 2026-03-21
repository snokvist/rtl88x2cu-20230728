# Cooperative RX: Primary Priority Investigation

## Problem

When both primary and helper receive the same WiFi frame, the helper's
drain tasklet often processes its copy BEFORE the primary's normal RX
path claims the frame via `recv_decache()`. This causes:

1. Helper frame undergoes SW AES-CCMP decrypt (expensive, even with
   kernel crypto acceleration)
2. Helper claims the seq_ctrl cache entry (`WRITE_ONCE(*prxseq, ...)`)
3. Primary's `recv_decache()` then sees the entry as a duplicate
4. Primary drops its HW-decrypted copy (which was free)

Net effect: the system pays SW decrypt cost for frames that the primary
would have handled for free via HW crypto. This defeats the design
intent that helpers only fill in frames the primary missed.

## Root Cause: Tasklet Scheduling Timing

The helper's `submit_helper_frame()` calls `tasklet_schedule()` at
line 1312, which fires on the **next softirq cycle**. On single-core
or when softirqs are coalesced, this can run before the primary's
USB recv_tasklet completes its full pipeline:

```
Helper USB URB → submit_helper_frame → tasklet_schedule(drain)
  ↓ (next softirq)
Drain tasklet → reads *prxseq (not yet claimed) → WRITE_ONCE → decrypt → deliver
  ↓ (meanwhile or after)
Primary USB URB → recv_decache → reads *prxseq (already claimed!) → DROP
```

The issue is that `tasklet_schedule()` has **zero delay** — it just
sets a pending flag. If the drain tasklet runs before the primary's
recv_decache, the helper wins the race and the expensive SW decrypt
path is taken.

## Observed Behavior

In testing, with cooperative RX active:
- The helper contribution shows high acceptance rates even when both
  adapters have good signal
- `helper_rx_dup_dropped` is relatively low compared to `helper_rx_accepted`
- The primary's own data throughput drops because its copies get
  discarded as duplicates

## Fix Options

### Option A: Delayed Drain via Timer (Recommended)

Replace `tasklet_schedule()` with a short timer-based delay that gives
the primary's normal RX path time to claim frames via `recv_decache()`.

**Mechanism:** Use `mod_timer()` with a 2-5ms delay. When the timer
fires, it schedules the drain tasklet. Multiple frame submissions
during the grace period naturally batch — the timer only fires once,
then the drain tasklet processes all pending frames in one batch.

```c
/* In submit_helper_frame(), replace:
 *   tasklet_schedule(&grp->coop_rx_tasklet);
 * With: */
if (!timer_pending(&grp->drain_timer))
    mod_timer(&grp->drain_timer,
              jiffies + msecs_to_jiffies(COOP_DRAIN_DELAY_MS));
```

Timer callback:
```c
static void coop_rx_drain_timer_fn(struct timer_list *t)
{
    struct cooperative_rx_group *grp =
        from_timer(grp, t, drain_timer);
    tasklet_schedule(&grp->coop_rx_tasklet);
}
```

**Tuning:** `COOP_DRAIN_DELAY_MS` should be:
- Long enough for the primary's USB pipeline to process (1-5ms typical)
- Short enough that video latency isn't visibly affected (<10ms)
- Start with 3ms (conservative), tune down based on testing

**Impact on frame delivery:**
- Primary frames (HW decrypted): delivered immediately, no change
- Helper frames (unique, primary missed): delayed by COOP_DRAIN_DELAY_MS
- Helper frames (duplicate): caught by dedup after delay, dropped (correct)

**Struct changes:**
```c
struct cooperative_rx_group {
    ...
    struct timer_list drain_timer;  /* delayed drain scheduling */
    ...
};
#define COOP_DRAIN_DELAY_MS  3  /* ms to wait for primary RX */
```

**Lifecycle:**
- `timer_setup(&grp->drain_timer, coop_rx_drain_timer_fn, 0)` in init
- `del_timer_sync(&grp->drain_timer)` in deinit/unbind (before tasklet_kill)

**Pros:**
- Clean, well-understood kernel API
- Natural batching (timer fires once for many frames)
- Primary always gets first shot at HW-decrypted frames
- Configurable delay via #define or module parameter

**Cons:**
- Adds 2-5ms latency to ALL helper frames (including unique ones)
- For a 4Mbps video stream, 3ms = ~1.5 frames delayed — acceptable

### Option B: Don't Write rxseq from Drain Tasklet

Remove the `WRITE_ONCE(*prxseq, seq_ctrl)` from the drain tasklet
(line 1419). This means the drain tasklet never claims frames in the
seq_ctrl cache — only the primary's `recv_decache()` writes.

**Effect:**
- Primary always claims frames (it's the only writer)
- Helper frames that are duplicates still get caught by:
  - The drain tasklet's `seq_ctrl == READ_ONCE(*prxseq)` check (primary
    wrote it)
  - The CCMP PN replay check during decrypt
  - The reorder window check (indicate_seq)
- Helper frames that are unique (primary missed) are NOT caught by the
  seq_ctrl check (primary never wrote it), so they proceed to decrypt
  and delivery

**Problem:** If the primary and helper both receive a frame but the
primary hasn't processed yet when the drain tasklet reads *prxseq,
the helper sees an old value (not a match) and proceeds to decrypt.
Then the primary also processes its copy. Both deliver. For encrypted
frames, the PN replay check catches one of the two. For unencrypted
frames, both could deliver — duplicate to the stack.

**This is actually the SAME behavior as the current race window** that
the code already documents (comment at line 1409-1418). The PN replay
and higher-layer dedup (IP/TCP) handle it.

**Pros:**
- Zero latency added to helper frames
- Primary always wins when it processes first
- Simpler than timer approach

**Cons:**
- When primary is slow (USB contention, heavy load), more duplicates
  may leak to the stack (but same as current race window)
- Non-QoS unencrypted traffic has no PN check — relies on TCP/IP dedup

### Option C: Hybrid — Delayed Drain + No Write

Combine Options A and B:
1. Add the drain timer delay (2-3ms)
2. Remove the `WRITE_ONCE` from the drain tasklet
3. The primary almost always processes first (gets 2-3ms head start)
4. The drain tasklet reads the primary's cached value and drops dups
5. Only truly missed frames (primary never received) pass through

**This is the strongest approach** because the delay ensures the primary
processes first in >99% of cases, and removing the WRITE_ONCE means
the drain tasklet never steals frames from the primary.

## Conclusion: No Change Needed (Dead End)

**Investigated 2025-03-21. Verdict: not worth changing.**

The kernel crypto API fix (commit 2b7b375) reduced helper SW decrypt
from ~80-100µs to ~2-5µs per frame on ARM64 CE. At 800fps, the helper
winning the race costs 0.2-0.4% CPU — negligible for any target.

Adding a timer delay to prioritize primary would ADD latency to the FPV
stream (3ms+) to save 0.2% CPU. That's the wrong trade-off for a
latency-critical video application.

**Why the helper is inherently faster:** The RTL8812CU firmware does less
work in monitor mode (no CAM lookup, no HW decrypt, no association
checks). The raw frame exits firmware and hits USB before the primary's
firmware finishes HW decrypting. This is a firmware-level timing
difference that can't be changed without adding artificial delay.

**Why removing WRITE_ONCE (Option B) doesn't help:** The helper still
wins the race regardless. Removing the write would let the primary also
process its copy, but the CCMP PN replay check catches the duplicate
during decrypt. Net effect: same behavior with slightly more wasted work
in the primary path. Not worth the change.

**Multiple helpers:** Only one helper copy is SW-decrypted per unique
WiFi frame. The drain tasklet's seq_ctrl dedup catches subsequent
helper copies. Adding more helpers does NOT multiply decrypt cost.

**Status: CLOSED. Revisit only if kernel crypto becomes unavailable
on a target platform (falling back to pure C AES would re-open the
CPU cost concern).**

---

*The original analysis below is preserved for reference.*

## Original Analysis (Options A/B/C)

### Option A — Delayed Drain (REJECTED: adds FPV latency)

## Recommendation (SUPERSEDED)

**Option C (Hybrid)** was the original recommendation but is now
**superseded by the "no change needed" conclusion above**:

1. Add `struct timer_list drain_timer` with 3ms delay
2. Replace `tasklet_schedule()` in submit_helper_frame with `mod_timer()`
3. Remove `WRITE_ONCE(*prxseq, seq_ctrl)` from drain tasklet
4. Keep all existing dedup layers (seq_ctrl READ, PN check, reorder window)

Expected outcome:
- In normal operation: primary HW-decrypts >99% of frames (free)
- Helper only SW-decrypts frames the primary genuinely missed
- `helper_rx_dup_dropped` increases significantly (most dups caught)
- `helper_rx_accepted` decreases to only unique contributions
- Overall CPU load drops (fewer SW decrypts)
- Video latency increase: ~3ms on helper-only frames (imperceptible)

## Verification Plan

1. Before fix: record `helper_rx_accepted` rate and CPU usage
2. After fix: same measurements
3. Expected: `helper_rx_accepted` drops from ~95% to ~5-20% of candidates
   (depending on signal conditions)
4. Expected: CPU from drain tasklet drops proportionally
5. Test with drop_primary=1: helper should still work (timer fires, drain
   processes), just with 3ms added latency
6. Test removing one adapter: primary-only works normally, no timer overhead
7. Verify kernel stats rx_bytes now show primary contribution (HW path
   increments kernel counters)

## Key Code Locations

| File | Line | What |
|---|---|---|
| `core/rtw_cooperative_rx.c:1312` | `tasklet_schedule()` | Replace with `mod_timer()` |
| `core/rtw_cooperative_rx.c:1419` | `WRITE_ONCE(*prxseq)` | Remove (Option C) |
| `core/rtw_cooperative_rx.c:1328` | `drain_tasklet()` | No change (reads *prxseq) |
| `core/rtw_cooperative_rx.c:321` | `tasklet_init()` | Add `timer_setup()` |
| `core/rtw_cooperative_rx.c:350` | `tasklet_kill()` | Add `del_timer_sync()` |
| `include/rtw_cooperative_rx.h` | group struct | Add `struct timer_list drain_timer` |
| `core/rtw_recv.c:896` | `recv_decache()` | No change (primary claims as before) |
