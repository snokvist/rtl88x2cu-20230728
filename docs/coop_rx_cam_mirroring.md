# CAM Mirroring — HW Decrypt for Helper Adapters

## Status: Investigation / Prototype Required

## Problem

With kernel crypto (CONFIG_COOP_RX_KERNEL_CRYPTO) enabled, per-frame AES-CCMP
decrypt dropped from ~80-100 µs to ~2-5 µs on Cortex-A55. This was a major
improvement, but with 2 helpers at video rates (4000+ fps aggregate), crypto
still consumes ~8-20 ms/s of single-core time — plus overhead from per-frame
`aead_request_alloc`, scatterlist setup, and AAD/nonce construction.

**If we could get HW decrypt on helper frames, the per-frame crypto cost
drops to zero.** This is the single largest remaining optimization.

## Background: Why HW Decrypt "Doesn't Work" in Monitor Mode

The existing analysis (`coop_rx_cpu_load_findings.md:73-87`) concluded that
HW decrypt on the helper is "architecturally impossible" because:

1. Helper runs in monitor mode — no STA association, no CAM entries
2. RCR is set to `BIT_AAP` (promiscuous) — assumed to bypass CAM lookup
3. Even if CAM were programmed, HW only matches RA == own MAC
4. Helper frames have RA = primary's MAC, not helper's MAC

**Points 1 and 2 are correct but fixable. Points 3 and 4 need verification.**

## Key Discovery: SECCFG Is Not Disabled in Monitor Mode

Reviewing `rtl8822c_set_mon_reg()` (rtl8822c_ops.c:1155-1193), when entering
monitor mode the driver modifies:

- `REG_RCR` → `BIT_AAP | BIT_APP_PHYSTS` (address filtering off)
- `REG_RXFLTMAP0/1/2` → `0xFFFF` (accept all frame subtypes)
- `REG_RX_DRVINFO_SZ` → sniffer bit set

**It does NOT touch `REG_SECCFG`.** The hardware crypto engine is never
explicitly disabled. The CAM lookup machinery remains active — it simply
has no entries to match against because the helper never associated.

## The Approach: CAM Mirroring

Program the helper's CAM with the primary's keys and peer MAC, then enable
the RX decrypt engine on the helper's hardware.

### How the RTL8822C CAM works

Each CAM entry stores (see `rtw_sec_write_cam_ent()`, hal_com.c:2546):

```
Entry[0]: ctrl(16) | MAC[0:1](16)    — Valid, Algorithm, KeyID, MAC bytes 0-1
Entry[1]: MAC[2:5](32)                — MAC bytes 2-5
Entry[2-5]: Key[0:15](128)            — 16-byte key material
Entry[6-7]: reserved/zero
```

The `ctrl` field: `BIT(15)=Valid | (algo << 2) | keyid`

In normal STA mode, `write_cam()` is called with:
- `mac` = peer's MAC (AP BSSID for STA, client MAC for AP)
- `key` = PTK or GTK key material
- This is the address the HW matches against frame TA (transmitter address)

### Implementation plan

During `rtw_coop_rx_bind_session()`, after the primary is associated and keys
are installed:

```c
/* Step 1: Read primary's key material */
struct sta_info *psta = rtw_get_stainfo(&primary->stapriv, grp->bound_bssid);
u8 *ptk = psta->dot118021x_UncstKey.skey;  /* pairwise key */

struct security_priv *psec = &primary->securitypriv;
u8 gtk_keyid = psec->dot118021XGrpKeyid;
u8 *gtk = psec->dot118021XGrpKey[gtk_keyid].skey;  /* group key */
u8 algo = psec->dot11PrivacyAlgrthm;  /* _AES_, _CCMP_256_, etc. */

/* Step 2: Program CAM on each HELPER adapter's hardware */
for (int i = 0; i < grp->num_helpers; i++) {
    _adapter *helper = grp->helpers[i];

    /* PTK entry: MAC = AP BSSID (peer), Key = primary's PTK */
    u16 ptk_ctrl = BIT(15) | ((algo & 0x07) << 2) | 0;  /* keyid=0 */
    write_cam(helper, 0, ptk_ctrl, grp->bound_bssid, ptk, algo & _SEC_TYPE_256_);

    /* GTK entry: MAC = broadcast, Key = primary's GTK */
    u8 bcast[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    u16 gtk_ctrl = BIT(15) | BIT(6) | ((algo & 0x07) << 2) | gtk_keyid;
    write_cam(helper, 4, gtk_ctrl, bcast, gtk, algo & _SEC_TYPE_256_);

    /* Step 3: Enable RX decrypt engine on helper */
    u8 scr = rtw_read8(helper, REG_SECCFG);
    scr |= SCR_RxDecEnable | SCR_CHK_KEYID;
    rtw_write8(helper, REG_SECCFG, scr);
}
```

### What happens when a frame arrives at the helper

1. HW receives all frames (BIT_AAP in RCR — unchanged)
2. For each frame, HW performs CAM lookup:
   - Extracts TA from 802.11 header
   - Scans CAM entries for matching MAC
   - If TA == `bound_bssid` → matches PTK entry → decrypts with PTK
   - If broadcast/multicast → matches GTK entry → decrypts with GTK
3. RX descriptor: `SWDEC=0` → `bdecrypted=1`
4. Coop RX code in `submit_helper_frame()` sees `bdecrypted=1`
5. Frame skips ALL decryption — no kernel crypto, no SW fallback

### Changes needed in coop RX code

If CAM mirroring is active and `bdecrypted=1`:

In `submit_helper_frame()` (rtw_cooperative_rx.c:1164-1183):
- The monitor-mode crypto fixup currently sets `encrypt` from primary's
  security context and `bdecrypted=0` when the Privacy bit is set
- With CAM mirroring: if `bdecrypted=1` (HW already decrypted), skip the
  fixup — leave `encrypt` and `bdecrypted` as-is from the RX descriptor
- Need to verify the RX descriptor correctly reports `encrypt` type when
  HW decrypts (it should — SWDEC=0 means the algo field is valid)

In `_coop_rx_drain_tasklet()` (rtw_cooperative_rx.c:1525-1556):
- The kernel crypto block checks `pa->encrypt && !pa->bdecrypted`
- If `bdecrypted=1`, this check naturally skips crypto → frame goes
  straight to reorder/indicate
- **No code changes needed in the drain tasklet**

New function needed:
- `coop_rx_cam_mirror_install()` — programs helper CAM entries from
  primary's key material. Called from `bind_session()`.
- `coop_rx_cam_mirror_clear()` — clears helper CAM on unbind/unpair.
- `coop_rx_cam_mirror_rekey()` — called on GTK rekey to update helper CAM.

### GTK rekey handling

When the AP rotates the group key (GTK rekey), the primary's
`set_group_key` handler is called. We need a hook to update the helper's
CAM with the new GTK. Two options:

1. **Hook `set_key` command handler** (rtw_mlme_ext.c:14843): After
   `write_cam()` for the primary, also call `coop_rx_cam_mirror_rekey()`
2. **Poll from drain tasklet**: Check if `psec->dot118021XGrpKeyid`
   changed since last install — simpler but adds per-frame overhead

Option 1 is cleaner. The GTK rekey path in `set_key_hdl()` already
has the new key material and cam_id.

## Key Unknowns — Must Be Tested

| # | Question | Risk | How to verify |
|---|----------|------|---------------|
| 1 | Does CAM lookup fire when BIT_AAP is set in RCR? | **Medium** — this is the make-or-break question. RCR controls address filtering; CAM is a separate engine, but they might be coupled. | Program CAM + check `bdecrypted` on received frames |
| 2 | Does CAM match on TA (transmitter) or RA (receiver)? | Low — `write_cam` usage consistently uses peer MAC, which is TA in STA mode | Same test — if decrypt works, TA match is confirmed |
| 3 | Can `write_cam()` succeed on a non-associated adapter? | Low — it's just MMIO register writes to the chip | Try it, check return value |
| 4 | Does `REG_SECCFG` enable actually activate the engine without association? | Low-Medium — the engine might check internal state beyond SECCFG | Set it and observe |
| 5 | Does the RX descriptor correctly report `encrypt` type when HW decrypts in this mode? | Low — standard HW behavior when SWDEC=0 | Check pattrib->encrypt after HW decrypt |

## Testing Plan

### Phase 1: Minimal probe (no code changes to coop RX logic)

Add temporary debug code to `bind_session()`:

```c
/* After bind completes successfully: */
RTW_INFO("coop_rx: CAM mirror test — programming helper CAM\n");

/* Program one PTK entry on helper */
_adapter *helper = grp->helpers[0];
u16 ctrl = BIT(15) | ((_AES_ & 0x07) << 2);
write_cam(helper, 0, ctrl, grp->bound_bssid, ptk, 0);

/* Enable RX decrypt */
u8 scr = rtw_read8(helper, REG_SECCFG);
RTW_INFO("coop_rx: helper SECCFG before: 0x%02x\n", scr);
scr |= 0x0C;  /* SCR_RxDecEnable | SCR_TxEncEnable */
rtw_write8(helper, REG_SECCFG, scr);
RTW_INFO("coop_rx: helper SECCFG after: 0x%02x\n",
         rtw_read8(helper, REG_SECCFG));
```

Add debug print in `submit_helper_frame()` (first 10 frames only):

```c
static int cam_dbg_count = 0;
if (cam_dbg_count < 10) {
    RTW_INFO("coop_rx: helper frame encrypt=%d bdecrypted=%d privacy=%d\n",
             pattrib->encrypt, pattrib->bdecrypted, pattrib->privacy);
    cam_dbg_count++;
}
```

### Expected outcomes

**If bdecrypted=1 appears:**
- CAM mirroring works. Proceed to full implementation.
- Expected CPU savings: eliminates ~2-5 µs per helper frame of kernel
  crypto overhead, plus eliminates aead_request_alloc/free overhead.
- At 4000 fps: saves ~8-20+ ms/s of CPU time.

**If bdecrypted=0 persists (encrypt=0 from RX descriptor):**
- CAM lookup is bypassed when BIT_AAP is set.
- Try alternative: don't use full monitor mode. Instead, keep the helper
  in a pseudo-managed state with RCR_AAP set but not via `set_mon_reg`.
  Manually configure RCR with `BIT_AAP | BIT_APM | BIT_APP_MIC | BIT_APP_ICV`
  (include the crypto-related RCR bits that normal managed mode uses).

**If bdecrypted=0 persists but encrypt != 0:**
- HW recognizes encryption but can't match CAM. Might need to set
  helper's own MAC to primary's MAC for RA matching. Test with
  `rtw_set_mac_addr_hw(helper, primary_mac)`.

## Relationship to Other Optimizations

CAM mirroring is **orthogonal** to the other performance blockers
documented in `coop_rx_performance_plan.md`. If CAM mirroring works:

- **Blocker #1 (aead_request_alloc)**: Eliminated — no crypto calls needed
- **Blocker #2 (single drain tasklet)**: Still relevant but less critical —
  per-frame cost in tasklet drops from ~10 µs to ~3-5 µs (dedup + reorder
  only, no crypto). Scaling limit roughly doubles.
- **Blocker #3 (SKB alloc + memcpy)**: Unchanged — still needed for USB
  deaggregation regardless of decrypt method.

If CAM mirroring does NOT work, all three blockers should be addressed
in parallel. See `coop_rx_performance_plan.md`.

## Code Locations Reference

| What | File | Lines |
|------|------|-------|
| Monitor mode RCR setup | hal/rtl8822c/rtl8822c_ops.c | 1155-1193 |
| SECCFG register writes | hal/rtl8822c/rtl8822c_ops.c | 1859-1877 |
| SECCFG in HW_VAR_SEC_CFG | hal/hal_com.c | 14459-14475 |
| CAM entry write | hal/hal_com.c | 2546-2600 |
| write_cam wrapper | core/rtw_wlan_util.c | 710-718 |
| PTK location | sta_info→dot118021x_UncstKey.skey | |
| GTK location | securitypriv→dot118021XGrpKey[keyid].skey | |
| Monitor crypto fixup | core/rtw_cooperative_rx.c | 1164-1183 |
| Drain tasklet decrypt | core/rtw_cooperative_rx.c | 1525-1556 |
| Bind session | core/rtw_cooperative_rx.c | 651-737 |
| Set key handler | core/rtw_mlme_ext.c | 14843-14890 |
| RX desc SWDEC bit | hal/rtl8822c/rtl8822c_ops.c | 4060 |
| bdecrypted check | core/rtw_recv.c | 574-746 |
