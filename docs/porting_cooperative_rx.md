# Porting Cooperative RX to Other Realtek Vendor Drivers

Step-by-step guide for adding cooperative RX diversity to another Realtek
vendor driver (rtl8812au, rtl8812eu, rtl88x2eu, etc.) from the same
generation. Tested source: `rtl88x2cu-20230728`. Target repos: any
[libc0607](https://github.com/libc0607) Realtek vendor driver.

**Time estimate:** ~30 minutes for someone familiar with the driver.

**Prerequisite:** Two identical USB adapters using the same chipset/driver.
Cross-driver cooperation (e.g. 8822CU + 8812EU) is not possible — see
`cooperative_rx_design.md` section 8 for details.

---

## Step 1: Copy core files (unchanged)

```bash
# From the rtl88x2cu-20230728 source tree, copy these two files:
cp core/rtw_cooperative_rx.c   <target>/core/
cp include/rtw_cooperative_rx.h <target>/include/
```

These files contain ALL cooperative RX logic: lifecycle, pairing, monitor
mode setup, 802.11 header parsing, SW decrypt fixup, PN pre-check, frame
injection, sysfs/debugfs interfaces, channel switch following. No
modification needed.

---

## Step 2: Makefile — add the new object file

Find the `rtk_core` variable in the target Makefile and add:

```makefile
rtk_core += core/rtw_cooperative_rx.o
```

Search for a line like `rtk_core += core/rtw_recv.o` and add it nearby.

---

## Step 3: Module parameter — `os_dep/linux/os_intfs.c`

### 3a. Add the include

Near the top includes:

```c
#include <rtw_cooperative_rx.h>
```

### 3b. Declare the module parameter

Find the module parameter section (search for `module_param`) and add:

```c
int rtw_cooperative_rx = 0;
module_param(rtw_cooperative_rx, int, 0644);
MODULE_PARM_DESC(rtw_cooperative_rx, "Enable cooperative RX diversity (0=off, 1=on)");
```

**Note:** `rtw_cooperative_rx` is declared as `extern` in the header
and defined in `rtw_cooperative_rx.c`. If the target driver's convention
is to define module params in `os_intfs.c` instead, move the `int
rtw_cooperative_rx = 0;` definition from `rtw_cooperative_rx.c` to
`os_intfs.c` and keep the `extern` in the header. For libc0607 drivers,
leaving it in `rtw_cooperative_rx.c` works fine.

### 3c. Export `rtw_netdev_ops`

Find:
```c
static const struct net_device_ops rtw_netdev_ops = {
```

Remove `static`:
```c
const struct net_device_ops rtw_netdev_ops = {
```

### 3d. Add sysfs init/deinit

Find `rtw_ndev_init` or the function that calls `register_netdevice` /
`register_netdev` and add sysfs setup nearby. Search for existing sysfs
group creation patterns in the file. Add after netdev registration:

```c
rtw_coop_rx_sysfs_init(ndev);
```

And in the teardown/unregister path:
```c
rtw_coop_rx_sysfs_deinit(ndev);
```

**Tip:** Search for `sysfs_create_group` in the file to find the pattern.

---

## Step 4: Init/deinit lifecycle — `os_dep/linux/usb_intf.c`

### 4a. Add the include

```c
#include <rtw_cooperative_rx.h>
```

### 4b. Init before USB registration

In `rtw_drv_entry()`, find `usb_register(&usb_drv.usbdrv)` and add
**before** it:

```c
	rtw_coop_rx_init();
```

### 4c. Deinit in module exit

In `rtw_drv_halt()`, add:

```c
	rtw_coop_rx_deinit();
```

### 4d. Deinit in error path

In `rtw_drv_entry()`, find the error handling block after `usb_register()`
fails (the `if (ret != 0)` block) and add inside it:

```c
		rtw_coop_rx_deinit();
```

### 4e. Remove adapter on disconnect

Find the USB disconnect handler (search for `rtw_coop_rx_remove_adapter`
pattern — look for `rtw_dev_remove` or similar). Add before the adapter
is freed:

```c
	if (padapter)
		rtw_coop_rx_remove_adapter(padapter);
```

---

## Step 5: RX path hook — `core/rtw_recv.c`

### 5a. Add the include

```c
#include <rtw_cooperative_rx.h>
```

### 5b. Add the one-line hook in `pre_recv_entry()`

Find the `pre_recv_entry()` function. Search for the monitor mode check:
```c
if (MLME_IS_MONITOR(primary_padapter))
```
or
```c
#ifdef CONFIG_WIFI_MONITOR
```

Add **before** that check (the cooperative hook must run first):

```c
	/* Cooperative RX: redirect helper data frames to primary's RX path */
	if (rtw_coop_rx_pre_recv_entry(precvframe, primary_padapter,
				       pbuf, pphy_status) == RTW_RX_HANDLED) {
		ret = _SUCCESS;
		goto exit;
	}
```

**Variable names:** `precvframe`, `primary_padapter`, `pbuf`, and
`pphy_status` should already exist at this point in `pre_recv_entry()`.
Verify the variable names match the target driver — the names are
consistent across libc0607 repos but check to be sure.

### 5c. Add function declarations in `include/rtw_recv.h`

Before `#endif` at the end of the file, add:

```c
/* PN replay check — used by cooperative RX pre-decrypt filter */
#define PN_LESS_CHK(a, b)	(((a-b) & 0x800000000000ULL) != 0)
#define VALID_PN_CHK(new, old)	(((old) == 0) || PN_LESS_CHK(old, new))

/* Exposed for cooperative RX merge path */
int recv_indicatepkt_reorder(_adapter *padapter, union recv_frame *prframe);
int recv_func_posthandle(_adapter *padapter, union recv_frame *prframe);
int recv_process_mpdu(_adapter *padapter, union recv_frame *prframe);
```

**Note:** `PN_LESS_CHK` and `VALID_PN_CHK` may already be defined locally
inside `rtw_recv.c`. Identical macro redefinitions are fine in C — no
conflict.

---

## Step 6: Channel switch hooks

### 6a. `core/rtw_cmd.c` — CSA path

Add the include:
```c
#include <rtw_cooperative_rx.h>
```

Find `rtw_dfs_ch_switch_hdl()`. Inside the STA-only branch (no AP/MESH),
find:
```c
		set_channel_bwmode(pri_adapter, req_ch, req_offset, req_bw);
		...
		rtw_rfctl_update_op_mode(rfctl, 0, 0, 0);
```

Add after `rtw_rfctl_update_op_mode`:
```c
		/* Move cooperative RX helpers to the new channel */
		rtw_coop_rx_notify_channel_switch(pri_adapter);
```

### 6b. `core/rtw_mlme_ext.c` — explicit channel set

Find `set_ch_hdl()`. After `rtw_rfctl_update_op_mode()`, add:

```c
	/* Move cooperative RX helpers to the new channel if primary switched */
	rtw_coop_rx_notify_channel_switch(padapter);
```

This file should already have `#include <rtw_cooperative_rx.h>` via
`drv_types.h`, but add it explicitly if needed.

---

## Step 7: Optional — silence log spam

### 7a. `core/rtw_swcrypto.c` — CCMP/GCMP decrypt failures

Find:
```c
RTW_INFO("Failed to decrypt CCMP(%u) frame", key_len);
```
Change to:
```c
RTW_DBG("Failed to decrypt CCMP(%u) frame", key_len);
```

Same for the GCMP line:
```c
RTW_INFO("Failed to decrypt GCMP(%u) frame", key_len);
```
→
```c
RTW_DBG("Failed to decrypt GCMP(%u) frame", key_len);
```

### 7b. `os_dep/linux/ioctl_cfg80211.c` — polling log spam

Find `cfg80211_rtw_get_txpower` and change its `RTW_INFO` calls to
`RTW_DBG`. Same for `cfg80211_rtw_get_channel`.

---

## Step 8: Build and verify

```bash
# Build
make -j$(nproc)

# Load with cooperative RX enabled
sudo insmod <module>.ko rtw_cooperative_rx=1

# Verify sysfs appears
ls /sys/class/net/wlan*/coop_rx/

# Verify debugfs appears
cat /sys/kernel/debug/rtw_coop_rx/stats

# Verify module parameter
cat /sys/module/<module>/parameters/rtw_cooperative_rx
```

---

## Quick reference: what each file needs

| File | Lines added | Purpose |
|------|-------------|---------|
| `core/rtw_cooperative_rx.c` | (copy) | All cooperative RX logic |
| `include/rtw_cooperative_rx.h` | (copy) | Structs, enums, API |
| `Makefile` | 1 | Build the new .o |
| `os_dep/linux/os_intfs.c` | ~5 | Module param, export ops, sysfs |
| `os_dep/linux/usb_intf.c` | ~5 | Init, deinit, error path, disconnect |
| `core/rtw_recv.c` | 5 | Include + one-line hook |
| `include/rtw_recv.h` | 7 | Function externs + PN macros |
| `core/rtw_cmd.c` | 2 | CSA channel switch hook |
| `core/rtw_mlme_ext.c` | 1 | Explicit channel switch hook |
| `core/rtw_swcrypto.c` | 2 | (optional) quiet decrypt logs |
| `os_dep/linux/ioctl_cfg80211.c` | 3 | (optional) quiet polling logs |

**Total hooks: ~28 lines across 7 files** (excluding optional noise reduction).

---

## Testing checklist

After porting, verify:

1. `make -j$(nproc)` — no new warnings in cooperative_rx files
2. `insmod <module>.ko rtw_cooperative_rx=1` — loads cleanly
3. `cat /sys/kernel/debug/rtw_coop_rx/stats` — shows IDLE state
4. Pair two adapters, bind session — state goes to ACTIVE
5. `helper_rx_candidates` incrementing (helper receiving)
6. `helper_rx_crypto_err` stays at 0 (SW decrypt working)
7. Ping works with cooperative RX active
8. `echo "eth0" > coop_rx_pair` returns error (sysfs validation)
9. `hostapd_cli chan_switch` — helper follows (dmesg shows notification)
10. Stats reset: `echo 1 > coop_rx_reset_stats` clears counters
