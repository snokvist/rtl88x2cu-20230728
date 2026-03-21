# Cooperative RX CPU Load Investigation

## Problem Statement

User testing on Radxa Zero 3 (4-core Cortex-A55 @ 1.4 GHz) revealed
excessive CPU load when cooperative RX is active with pairing enabled:

| Configuration | CPU Load |
|---|---|
| `rtw_cooperative_rx=0` (or unpatched driver) | Normal |
| `rtw_cooperative_rx=1`, no binding | Normal |
| `rtw_cooperative_rx=1`, binding active (`echo 1 > coop_rx_pair`) | **High** |
| `rtw_cooperative_rx=1`, binding active, `drop_primary=1` | **System overload, glitching** |

This was not observed during development testing on a 16-core x86 machine
with AES-NI hardware acceleration.

## Root Cause

**Every helper frame undergoes software AES-CCMP decryption using a pure C
table-lookup implementation (Rijndael reference code) that does not use
ARMv8 Cryptography Extensions.**

### Why SW decrypt is required

The helper adapter runs in **monitor mode** to receive all frames on the
channel. In monitor mode:

1. The RTL8812CU's HW crypto engine (CAM — Content Addressable Memory) is
   bypassed because there is no STA association on the helper
2. The RX descriptor reports `encrypt=0` even for encrypted frames
3. The coop RX code detects encryption via the 802.11 Protected Frame bit
   (`pattrib->privacy`) and populates crypto fields from the primary's
   security context (`rtw_cooperative_rx.c:897-916`)
4. Frames arrive at the drain tasklet with `encrypt != 0`, `bdecrypted == 0`
5. `recv_func_posthandle()` → `decryptor()` → `rtw_aes_decrypt()` →
   `_rtw_ccmp_decrypt()` performs full AES-CCM in software

### The crypto implementation

The driver ships its own AES (`core/crypto/aes-internal.c`), a pure C
Rijndael implementation by Jouni Malinen. It does NOT use:

- Linux kernel crypto API (`crypto_alloc_skcipher` etc.)
- ARMv8 Cryptography Extensions (AES/PMULL instructions)
- NEON SIMD acceleration
- Any hardware acceleration

This is standard for out-of-tree WiFi drivers that predate the kernel's
unified crypto framework, but it means AES-CCMP decrypt is **~10-50x
slower** than kernel crypto with CE on Cortex-A55.

### Cost analysis on Cortex-A55

| Component | Per-frame cost | Notes |
|---|---|---|
| AES-CCMP SW decrypt (pure C) | ~80-100 µs | Dominant cost |
| Frame validation + enqueue | ~3 µs | RCU, memcmp, list ops |
| Drain tasklet dedup + reorder | ~1 µs | seq_ctrl compare, queue |
| **Total per helper frame** | **~85-105 µs** | |

At typical video stream rates:

| Frame rate | SW decrypt CPU time | CPU % (single A55 core) |
|---|---|---|
| 800 fps | 80 ms/s | 8% |
| 2000 fps (AMPDU burst) | 200 ms/s | 20% |
| 4000 fps (heavy traffic) | 400 ms/s | 40% |

With `drop_primary=1`, 100% of frames take the helper path, doubling the
load vs normal operation where only missed frames need SW decrypt.

### Why HW decrypt on the helper is not feasible

The RTL8812CU's HW crypto engine works via CAM (Content Addressable Memory)
lookup: incoming frames are matched against a table of (MAC address, key)
entries programmed during STA association. In monitor mode:

1. No STA association → no CAM entries programmed
2. RCR (Receive Configuration Register) is set to promiscuous mode
3. The HW skips CAM lookup entirely for all received frames
4. Even if we programmed a CAM entry manually, the HW only does CAM lookup
   for frames where RA matches the adapter's own MAC — helper frames have
   RA = primary's MAC, not helper's MAC

**Conclusion: HW-accelerated decrypt on the helper adapter is architecturally
impossible with the RTL8812CU chipset in monitor mode.**

## Fix Options (Prioritized)

### Option A: Kernel Crypto API with ARMv8 CE (Primary recommendation)

**Goal:** Replace the driver's pure C AES with the Linux kernel's crypto
subsystem, which automatically uses ARMv8 Cryptography Extensions on
Cortex-A55/A53/A76 processors.

**Expected improvement:** 10-50x faster AES-CCMP decrypt. The Cortex-A55
has dedicated `AESE`/`AESMC` instructions that process 16 bytes per cycle.
Per-frame cost drops from ~80-100 µs to ~2-5 µs.

**Implementation approach:**

1. Replace `_rtw_ccmp_decrypt()` with a wrapper that uses
   `crypto_alloc_aead("ccm(aes)")` from the kernel crypto API
2. The kernel's `ccm(aes)` automatically dispatches to:
   - `aes-arm64-ce` on ARMv8 with CE (Cortex-A55, A53, A76)
   - `aes-arm` (NEON bitsliced) on ARMv7/ARMv8 without CE
   - `aes-generic` (C fallback) on x86 without AES-NI
   - `aes-ni` on x86 with AES-NI
3. Pre-allocate the `crypto_aead` transform once per group (not per-frame)
4. Use the existing key material from `stainfo->dot118021x_UncstKey` and
   `psecuritypriv->dot118021XGrpKey`

**Key design decisions:**
- The kernel crypto API can sleep during `crypto_alloc_aead()` but NOT
  during `crypto_aead_decrypt()` — safe for softirq/tasklet context
- The transform must be allocated during pairing (process context) and
  stored in the cooperative group
- CCMP nonce/AAD construction stays in driver code (reuse existing
  `ccmp_aad_nonce()` logic)
- The actual AES block cipher is handled by kernel, getting HW acceleration
  transparently

**Scope of change:**
- New file or section in `rtw_cooperative_rx.c` for kernel-crypto wrapper
- Modify `recv_func_posthandle()` path to use kernel crypto when called
  from drain tasklet (can check `pframe->u.hdr.adapter != pframe's originating adapter`
  or a flag)
- Alternative: create a parallel `coop_rx_decrypt()` function called only
  from the drain tasklet, leaving the normal RX path unchanged

**Risk:** Low. The kernel crypto API is stable and well-tested. The wrapper
is isolated to coop RX frames; primary's normal HW-decrypted path is unchanged.

**Prerequisites:**
- Kernel must have `CONFIG_CRYPTO_CCM` and `CONFIG_CRYPTO_AES_ARM64_CE`
  (standard on all modern ARM64 kernels, including Armbian/Radxa)
- Verify with: `cat /proc/crypto | grep -A4 "name.*aes"` on the target

### Option B: Tighter pre-decrypt dedup (Complementary)

**Goal:** Drop helper frames that the primary already processed BEFORE
they reach the expensive SW decrypt path.

**Current dedup layers and their limitations:**

| Layer | Location | What it catches | Gap |
|---|---|---|---|
| PN pre-check | `submit_helper_frame:971-994` | Frames with PN ≤ primary's cached PN | Only works if primary processed first AND updated the PN cache |
| Seq_ctrl dedup | `drain_tasklet:1143-1188` | Frames where primary already updated rxseq | Same race: only works if primary was first |
| Reorder window | `recv_indicatepkt_reorder` | In-window seq collisions | After decrypt — too late |

**The gap:** When helper and primary process the same frame near-simultaneously,
neither dedup layer catches the duplicate because neither has updated its
cache yet. Both frames proceed to decrypt.

**Proposed enhancement — reorder window pre-check:**

Before decrypting in the drain tasklet, check if the primary's reorder
window already has a frame in this sequence number's slot:

```c
/* In drain_tasklet, before recv_func_posthandle(): */
if (pa->qos && psta) {
    struct recv_reorder_ctrl *preorder = &psta->recvreorder_ctrl[pa->priority];
    if (preorder->enable) {
        u16 wstart = preorder->indicate_seq;
        u16 wend = (wstart + preorder->wsize_b - 1) & 0xFFF;
        if (SN_EQUAL(pa->seq_num, wstart) || /* already indicated */
            SN_LESS(pa->seq_num, wstart)) {  /* behind window */
            /* Primary already consumed or indicated this frame */
            drop_and_continue;
        }
    }
}
```

**Expected improvement:** Catches 50-80% of duplicates before decrypt in
normal operation (when both primary and helper are active). Does NOT help
with `drop_primary=1` (no primary frames to advance the window).

**Scope:** ~20 lines in `rtw_coop_rx_drain_tasklet()`, before the
`recv_func_posthandle()` call.

### Option C: ARM NEON AES (Fallback if kernel crypto unavailable)

**Goal:** If the kernel crypto API is not available (very old kernels,
custom builds without crypto modules), add a NEON-accelerated AES
implementation directly in the driver.

**Approach:** Replace `core/crypto/aes-internal.c` AES block cipher with
ARMv8 NEON bitsliced AES. This is a drop-in replacement that provides
~4-8x speedup over pure C without requiring kernel crypto modules.

**Complexity:** Medium — requires assembly or intrinsics for ARM NEON,
plus architecture detection at build time. Less portable than Option A.

**When to use:** Only if Option A is blocked by kernel configuration
constraints on the target platform.

## Recommended Implementation Order

1. **Option A (kernel crypto)** — Primary fix. Maximum speedup (10-50x),
   transparent HW acceleration, minimal risk, isolated to coop RX path.

2. **Option B (tighter dedup)** — Complementary. Reduces decrypt volume
   by catching more dups pre-decrypt. Small change, low risk. Implement
   alongside or after Option A.

3. **Option C (NEON AES)** — Fallback only. Use if specific targets lack
   kernel crypto support.

## Verification Plan

1. **Measure baseline:** On Radxa Zero 3, with coop RX active and paired,
   record CPU usage per core (`mpstat 1`) and coop RX stats during a
   sustained video stream

2. **After Option A:** Same measurement. Expected: drain tasklet CPU drops
   from ~20-40% to ~1-3% per core. Helper-only mode (`drop_primary=1`)
   should be usable without system overload.

3. **After Option B:** Check `helper_rx_dup_dropped` counter increase
   relative to `helper_rx_candidates`. Target: >70% of dups caught
   pre-decrypt in normal mode.

4. **Kernel crypto check:** Run on target:
   ```bash
   cat /proc/crypto | grep -A4 "name.*ccm"
   modprobe ccm 2>/dev/null
   cat /proc/crypto | grep -A4 "name.*aes"
   # Look for "driver: aes-arm64-ce" or "aes-ce"
   ```

## Key Code Locations

| File | Lines | Purpose |
|---|---|---|
| `core/rtw_cooperative_rx.c:1192-1193` | Drain tasklet decrypt call | Where SW decrypt is invoked |
| `core/rtw_cooperative_rx.c:897-916` | Monitor-mode crypto fixup | Populates encrypt fields for helper frames |
| `core/rtw_cooperative_rx.c:971-994` | PN pre-check | First dedup layer (pre-drain) |
| `core/rtw_cooperative_rx.c:1143-1188` | Seq_ctrl dedup | Second dedup layer (in drain) |
| `core/rtw_recv.c:4237` | `decryptor()` call | Entry to SW decrypt |
| `core/rtw_security.c:2040-2129` | `rtw_aes_decrypt()` | SW AES-CCMP decrypt dispatcher |
| `core/crypto/aes-internal.c` | Full file | Pure C Rijndael — the bottleneck |
| `core/crypto/ccmp.c` | Full file | CCMP AAD/nonce + AES-CCM wrapper |
| `include/rtw_cooperative_rx.h:108-109` | `COOP_PENDING_MAX=128, COOP_BATCH_SIZE=32` | Tuning constants |
