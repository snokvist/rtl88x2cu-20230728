/******************************************************************************
 *
 * Copyright(c) 2024 Realtek Corporation.
 *
 * Cooperative RX Diversity Mode — Core Implementation (Open Network Only)
 *
 * Enables a helper USB adapter to contribute received frames to a primary
 * adapter's RX path, improving robustness under fading/interference.
 *
 * Architecture:
 *   - Primary adapter operates normally in STA or AP mode
 *   - Helper adapter(s) in monitor mode on the same channel
 *   - Helper RX frames are injected into primary's recv_func_posthandle()
 *   - Reorder window provides duplicate suppression for QoS traffic
 *   - Non-QoS dedup via small sequence number ring buffer
 *   - One coherent RX stream is delivered to the network stack
 *   - STA mode: captures AP->STA downlink (to_fr_ds=2)
 *   - AP mode: captures STA->AP uplink (to_fr_ds=1)
 *
 * This is the MINIMAL open-network-only build. No encryption, no crypto,
 * no CAM mirroring.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 *****************************************************************************/

#define pr_fmt(fmt) "rtw_coop_rx: " fmt

#include <drv_types.h>
#include <hal_data.h>
#include <rtw_recv.h>
#include <rtw_cooperative_rx.h>
#include <linux/rtnetlink.h>
#include <linux/version.h>
#include <net/net_namespace.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

/* Module parameter -- 0=disabled (default), 1=enabled */
int rtw_cooperative_rx = 0;

/* Debug: drop all primary RX data frames to test helper-only path */
int rtw_coop_rx_drop_primary = 0;

/* Global cooperative group singleton */
struct cooperative_rx_group *rtw_coop_rx_group = NULL;

/*
 * Reset all stats counters using atomic_set() -- safe for atomic_t fields.
 * Never use memset() on struct coop_rx_stats: atomic_t may have internal
 * state beyond a simple integer on some kernel configs (DEBUG_ATOMIC_SLEEP).
 */
static void coop_rx_stats_reset(struct coop_rx_stats *stats)
{
	atomic_set(&stats->helper_rx_candidates, 0);
	atomic_set(&stats->helper_rx_accepted, 0);
	atomic_set(&stats->helper_rx_dup_dropped, 0);
	atomic_set(&stats->helper_rx_pool_full, 0);
	atomic_set(&stats->helper_rx_foreign, 0);
	atomic_set(&stats->helper_rx_late, 0);
	atomic_set(&stats->helper_rx_no_sta, 0);
	atomic_set(&stats->helper_rx_deferred, 0);
	atomic_set(&stats->helper_rx_backpressure, 0);
	atomic_set(&stats->helper_rx_rssi_better, 0);
	atomic_set(&stats->helper_rx_rssi_worse, 0);
	atomic_set(&stats->fallback_events, 0);
	atomic_set(&stats->pair_events, 0);
	atomic_set(&stats->unpair_events, 0);
	atomic_long_set(&stats->helper_rx_bytes, 0);
}


/*
 * ============================================================
 * Lifecycle Management
 * ============================================================
 */

int rtw_coop_rx_init(void)
{
	struct cooperative_rx_group *grp;

	if (!rtw_coop_rx_enabled())
		return 0;

	grp = rtw_zmalloc(sizeof(*grp));
	if (!grp) {
		RTW_ERR("%s: failed to allocate cooperative group\n", __func__);
		return -ENOMEM;
	}

	spin_lock_init(&grp->lock);
	grp->state = COOP_STATE_IDLE;
	grp->primary = NULL;
	grp->num_helpers = 0;
	coop_rx_stats_reset(&grp->stats);

	/* Init deferred processing queue and tasklet */
	_rtw_init_queue(&grp->pending_queue);
	atomic_set(&grp->pending_count, 0);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
	tasklet_setup(&grp->coop_rx_tasklet, rtw_coop_rx_drain_tasklet);
#else
	tasklet_init(&grp->coop_rx_tasklet, rtw_coop_rx_drain_tasklet_compat,
		     (unsigned long)grp);
#endif

	/* Publish the group pointer */
	smp_wmb();
	WRITE_ONCE(rtw_coop_rx_group, grp);

	RTW_INFO("%s: cooperative RX group initialized\n", __func__);

#ifdef CONFIG_DEBUG_FS
	rtw_coop_rx_debugfs_init();
#endif

	return 0;
}

void rtw_coop_rx_deinit(void)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return;

#ifdef CONFIG_DEBUG_FS
	rtw_coop_rx_debugfs_deinit();
#endif

	/* Kill the drain tasklet FIRST -- ensures no in-flight processing */
	tasklet_kill(&grp->coop_rx_tasklet);

	/* Drain remaining pending frames back to primary's free pool */
	{
		union recv_frame *pframe;
		_adapter *primary = grp->primary;

		while ((pframe = rtw_alloc_recvframe(&grp->pending_queue)) != NULL) {
			atomic_dec(&grp->pending_count);
			if (primary) {
				rtw_free_recvframe(pframe,
					&primary->recvpriv.free_recv_queue);
			} else {
				/* Primary gone -- free skb directly to avoid leak */
				if (pframe->u.hdr.pkt) {
					dev_kfree_skb_any(pframe->u.hdr.pkt);
					pframe->u.hdr.pkt = NULL;
				}
			}
		}
	}

	spin_lock_irqsave(&grp->lock, flags);
	grp->state = COOP_STATE_DISABLED;
	grp->primary = NULL;
	grp->num_helpers = 0;
	spin_unlock_irqrestore(&grp->lock, flags);

	/* NULL the pointer first, then wait for readers, then free */
	WRITE_ONCE(rtw_coop_rx_group, NULL);
	synchronize_rcu();

	rtw_mfree((u8 *)grp, sizeof(*grp));

	RTW_INFO("%s: cooperative RX group destroyed\n", __func__);
}


/*
 * ============================================================
 * Adapter Pairing
 * ============================================================
 */

int rtw_coop_rx_set_primary(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;

	if (!rtw_coop_rx_enabled())
		return -ENODEV;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return -ENODEV;

	spin_lock_irqsave(&grp->lock, flags);

	if (grp->primary && grp->primary != adapter) {
		RTW_WARN("%s: primary already set to different adapter\n",
			 __func__);
		spin_unlock_irqrestore(&grp->lock, flags);
		return -EBUSY;
	}

	grp->primary = adapter;
	grp->primary_dvobj = adapter_to_dvobj(adapter);

	if (grp->state == COOP_STATE_DISABLED)
		grp->state = COOP_STATE_IDLE;

	spin_unlock_irqrestore(&grp->lock, flags);

	RTW_INFO("%s: primary adapter set (iface_id=%d, mac="MAC_FMT")\n",
		 __func__, adapter->iface_id,
		 MAC_ARG(adapter_mac_addr(adapter)));
	return 0;
}

int rtw_coop_rx_add_helper(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;
	int i;

	if (!rtw_coop_rx_enabled())
		return -ENODEV;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return -ENODEV;

	spin_lock_irqsave(&grp->lock, flags);

	if (!grp->primary) {
		RTW_WARN("%s: no primary adapter set\n", __func__);
		spin_unlock_irqrestore(&grp->lock, flags);
		return -EINVAL;
	}

	if (grp->num_helpers >= COOP_MAX_HELPERS) {
		RTW_WARN("%s: max helpers reached (%d)\n",
			 __func__, COOP_MAX_HELPERS);
		spin_unlock_irqrestore(&grp->lock, flags);
		return -ENOSPC;
	}

	/* Check for duplicate */
	for (i = 0; i < grp->num_helpers; i++) {
		if (grp->helpers[i] == adapter) {
			spin_unlock_irqrestore(&grp->lock, flags);
			return 0; /* already added */
		}
	}

	grp->helpers[grp->num_helpers] = adapter;
	grp->helper_dvobjs[grp->num_helpers] = adapter_to_dvobj(adapter);
	grp->num_helpers++;

	/* Mark adapter as helper for fast hot-path check */
	WRITE_ONCE(adapter->is_coop_helper, 1);

	if (grp->state == COOP_STATE_IDLE)
		grp->state = COOP_STATE_BINDING;

	atomic_inc(&grp->stats.pair_events);
	spin_unlock_irqrestore(&grp->lock, flags);

	RTW_INFO("%s: helper adapter added (iface_id=%d, mac="MAC_FMT
		 ", total_helpers=%d)\n",
		 __func__, adapter->iface_id,
		 MAC_ARG(adapter_mac_addr(adapter)),
		 grp->num_helpers);
	return 0;
}

int rtw_coop_rx_remove_helper(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;
	int i, found = 0;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return -ENODEV;

	spin_lock_irqsave(&grp->lock, flags);

	for (i = 0; i < grp->num_helpers; i++) {
		if (grp->helpers[i] == adapter) {
			found = 1;
			/* Shift remaining helpers down */
			for (; i < grp->num_helpers - 1; i++) {
				grp->helpers[i] = grp->helpers[i + 1];
				grp->helper_dvobjs[i] = grp->helper_dvobjs[i + 1];
			}
			grp->helpers[grp->num_helpers - 1] = NULL;
			grp->helper_dvobjs[grp->num_helpers - 1] = NULL;
			grp->num_helpers--;
			break;
		}
	}

	if (found) {
		atomic_inc(&grp->stats.unpair_events);
		if (grp->num_helpers == 0 && grp->state == COOP_STATE_ACTIVE) {
			grp->state = COOP_STATE_IDLE;
			atomic_inc(&grp->stats.fallback_events);
			RTW_INFO("%s: last helper removed, falling back to "
				 "primary-only mode\n", __func__);
		}
	}

	spin_unlock_irqrestore(&grp->lock, flags);

	/* Wait for any in-flight helper RX processing */
	if (found)
		synchronize_rcu();

	return found ? 0 : -ENOENT;
}

/*
 * Called when any adapter is being removed (USB disconnect, driver unload).
 * Safely removes it from the cooperative group regardless of role.
 */
void rtw_coop_rx_remove_adapter(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;

	if (!rtw_coop_rx_enabled())
		return;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return;

	/* Try removing as helper first */
	rtw_coop_rx_remove_helper(adapter);

	/* Check if it's the primary */
	spin_lock_irqsave(&grp->lock, flags);
	if (grp->primary == adapter) {
		spin_unlock_irqrestore(&grp->lock, flags);

		RTW_INFO("%s: primary adapter removed, tearing down "
			 "cooperative group\n", __func__);

		/* Kill tasklet and drain pending frames before clearing primary */
		tasklet_kill(&grp->coop_rx_tasklet);
		{
			union recv_frame *pframe;

			while ((pframe = rtw_alloc_recvframe(&grp->pending_queue)) != NULL) {
				atomic_dec(&grp->pending_count);
				rtw_free_recvframe(pframe,
					&adapter->recvpriv.free_recv_queue);
			}
		}

		spin_lock_irqsave(&grp->lock, flags);
		grp->primary = NULL;
		grp->primary_dvobj = NULL;
		grp->num_helpers = 0;
		memset(grp->helpers, 0, sizeof(grp->helpers));
		memset(grp->helper_dvobjs, 0, sizeof(grp->helper_dvobjs));
		grp->state = COOP_STATE_IDLE;
		atomic_inc(&grp->stats.fallback_events);
	}
	spin_unlock_irqrestore(&grp->lock, flags);
}

/*
 * Auto-discover helper adapters among all netdevices in the same
 * network namespace. Finds interfaces using rtw_netdev_ops that
 * are not the primary, and adds them as helpers.
 * Must be called from process context (takes rtnl_lock).
 */
static int rtw_coop_rx_auto_discover_helpers(_adapter *primary)
{
	struct net_device *ndev;
	struct net *net;
	int added = 0;

	if (!primary || !primary->pnetdev)
		return 0;

	net = dev_net(primary->pnetdev);

	rtnl_lock();
	for_each_netdev(net, ndev) {
		_adapter *candidate;

		if (ndev == primary->pnetdev)
			continue;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29))
		if (ndev->netdev_ops != &rtw_netdev_ops)
			continue;
#else
		continue; /* Cannot verify on old kernels */
#endif

		candidate = rtw_netdev_priv(ndev);
		if (!candidate)
			continue;

		if (rtw_coop_rx_add_helper(candidate) == 0)
			added++;
	}
	rtnl_unlock();

	if (added > 0)
		RTW_INFO("%s: auto-discovered %d helper(s)\n",
			 __func__, added);

	return added;
}


/*
 * ============================================================
 * Session Binding
 * ============================================================
 *
 * Bind cooperative session to primary's current BSS.
 * Called after primary successfully associates.
 * Simplified: no crypto init, no CAM mirror — just capture BSS
 * context, set state, and enable monitor on helpers.
 */

int rtw_coop_rx_bind_session(_adapter *primary)
{
	struct cooperative_rx_group *grp;
	struct mlme_priv *pmlmepriv;
	struct wlan_network *cur_network;
	unsigned long flags;
	int i;

	if (!rtw_coop_rx_enabled())
		return -ENODEV;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp || grp->primary != primary)
		return -ENODEV;

	pmlmepriv = &primary->mlmepriv;
	cur_network = &pmlmepriv->cur_network;

	if (check_fwstate(pmlmepriv, WIFI_AP_STATE)) {
		/* AP mode: considered "associated" once started */
		if (!check_fwstate(pmlmepriv, WIFI_ASOC_STATE)) {
			RTW_WARN("%s: AP not started\n", __func__);
			return -ENOTCONN;
		}
	} else if (check_fwstate(pmlmepriv, WIFI_STATION_STATE)) {
		if (!check_fwstate(pmlmepriv, WIFI_ASOC_STATE)) {
			RTW_WARN("%s: primary not associated\n", __func__);
			return -ENOTCONN;
		}
	} else {
		RTW_WARN("%s: unsupported mode (not STA or AP)\n", __func__);
		return -ENOTCONN;
	}

	spin_lock_irqsave(&grp->lock, flags);

	/* Capture BSS context.
	 * AP mode: BSSID = our own MAC.
	 * STA mode: BSSID = AP MAC (Addr2 in From-DS frames). */
	if (check_fwstate(pmlmepriv, WIFI_AP_STATE))
		_rtw_memcpy(grp->bound_bssid,
			     adapter_mac_addr(primary), ETH_ALEN);
	else
		_rtw_memcpy(grp->bound_bssid,
			     cur_network->network.MacAddress, ETH_ALEN);
	grp->bound_channel = primary->mlmeextpriv.cur_channel;
	grp->bound_bw = primary->mlmeextpriv.cur_bwmode;

	/* Reset non-QoS dedup cache */
	memset(&grp->nonqos_cache, 0, sizeof(grp->nonqos_cache));

	if (grp->num_helpers > 0)
		grp->state = COOP_STATE_ACTIVE;

	/* Snapshot helpers under lock, then release before calling
	 * functions that may sleep (rtw_setopmode_cmd). */
	{
		_adapter *helper_snap[COOP_MAX_HELPERS];
		int snap_count = grp->num_helpers;
		u8 ch = grp->bound_channel;

		for (i = 0; i < snap_count; i++)
			helper_snap[i] = grp->helpers[i];

		spin_unlock_irqrestore(&grp->lock, flags);

		for (i = 0; i < snap_count; i++) {
			if (helper_snap[i])
				rtw_coop_rx_enable_helper_monitor(
					helper_snap[i], ch);
		}
	}

	RTW_INFO("%s: session bound to BSSID="MAC_FMT" ch=%u bw=%u "
		 "(helpers=%d, state=%s)\n",
		 __func__, MAC_ARG(grp->bound_bssid),
		 grp->bound_channel, grp->bound_bw,
		 grp->num_helpers,
		 grp->state == COOP_STATE_ACTIVE ? "ACTIVE" : "BINDING");
	return 0;
}

void rtw_coop_rx_unbind_session(void)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return;

	/* Stop the drain tasklet FIRST */
	tasklet_disable(&grp->coop_rx_tasklet);

	/* Set state to IDLE before drain so submit_helper_frame() rejects
	 * new frames during the drain window (fixes orphaned frame race). */
	spin_lock_irqsave(&grp->lock, flags);
	if (grp->state == COOP_STATE_ACTIVE ||
	    grp->state == COOP_STATE_BINDING) {
		grp->state = grp->primary ? COOP_STATE_IDLE : COOP_STATE_DISABLED;
		memset(grp->bound_bssid, 0, ETH_ALEN);
		grp->bound_channel = 0;
	}
	spin_unlock_irqrestore(&grp->lock, flags);

	/* Drain pending frames with SKB leak fix */
	{
		union recv_frame *pframe;
		_adapter *primary = grp->primary;

		while ((pframe = rtw_alloc_recvframe(&grp->pending_queue)) != NULL) {
			atomic_dec(&grp->pending_count);
			if (primary) {
				rtw_free_recvframe(pframe,
					&primary->recvpriv.free_recv_queue);
			} else {
				if (pframe->u.hdr.pkt) {
					dev_kfree_skb_any(pframe->u.hdr.pkt);
					pframe->u.hdr.pkt = NULL;
				}
			}
		}
	}
	tasklet_enable(&grp->coop_rx_tasklet);

	RTW_INFO("%s: session unbound\n", __func__);
}


/*
 * ============================================================
 * Monitor Mode for Helper Adapters
 * ============================================================
 *
 * Enable monitor mode on a helper adapter via driver internals.
 * iw/cfg80211 set type monitor does NOT call the driver's change_iface
 * callback when the interface is down, so WIFI_MONITOR_STATE is never
 * set and the RCR stays closed.  This function uses the driver's own
 * APIs to properly enter monitor mode and park on the bound channel.
 */

int rtw_coop_rx_enable_helper_monitor(_adapter *helper, u8 channel)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);
	struct net_device *ndev = helper->pnetdev;
	u8 bw = CHANNEL_WIDTH_20;
	u8 offset = HAL_PRIME_CHNL_OFFSET_DONT_CARE;

#ifdef CONFIG_RTW_ACS
	/* Disable ACS BEFORE bringing the interface up -- ndo_open calls
	 * rtw_acs_start() when acs_mode is set. Clear it first so ACS
	 * never activates on the helper. */
	helper->registrypriv.acs_mode = 0;
#endif

	/* Ensure the interface is UP -- the driver's ndo_open handler
	 * initializes USB URBs and hardware state. Without this, the
	 * radio doesn't receive any frames. Safe to call if already UP. */
	if (!(ndev->flags & IFF_UP)) {
		rtnl_lock();
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
		dev_open(ndev, NULL);
#else
		dev_open(ndev);
#endif
		rtnl_unlock();
	}

	/* Set netdev type and cfg80211 wdev type under rtnl_lock to
	 * prevent races with rtnetlink queries and cfg80211. */
	rtnl_lock();
	ndev->type = ARPHRD_IEEE80211_RADIOTAP;
	if (helper->rtw_wdev)
		helper->rtw_wdev->iftype = NL80211_IFTYPE_MONITOR;
	rtnl_unlock();

	/* Set driver internal mode -- configures WIFI_MONITOR_STATE,
	 * sets RCR to promiscuous, opens RX filter maps */
	rtw_set_802_11_infrastructure_mode(helper, Ndis802_11Monitor, 0);
	rtw_setopmode_cmd(helper, Ndis802_11Monitor, RTW_CMDF_WAIT_ACK);

#ifdef CONFIG_RTW_ACS
	rtw_acs_stop(helper);
#endif

	/* Match primary's bandwidth so helper receives all sub-carriers */
	if (grp && grp->primary) {
		bw = grp->primary->mlmeextpriv.cur_bwmode;
		offset = grp->primary->mlmeextpriv.cur_ch_offset;
	}

	set_channel_bwmode(helper, channel, offset, bw);

	RTW_INFO("%s: helper iface_id=%d set to monitor mode ch=%u bw=%u\n",
		 __func__, helper->iface_id, channel, bw);
	return 0;
}


/*
 * ============================================================
 * Channel Switch Notification
 * ============================================================
 *
 * Called after the primary adapter's channel changes (CSA, roam, etc.).
 * If the new channel differs from the bound channel, move all helpers
 * to the new channel so they continue receiving frames.
 */

void rtw_coop_rx_notify_channel_switch(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	_adapter *helper_snap[COOP_MAX_HELPERS];
	int snap_count;
	unsigned long flags;
	u8 new_ch, old_ch, new_bw, new_offset;
	int i;

	if (!rtw_coop_rx_enabled())
		return;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return;

	spin_lock_irqsave(&grp->lock, flags);

	if (grp->state != COOP_STATE_ACTIVE || grp->primary != adapter) {
		spin_unlock_irqrestore(&grp->lock, flags);
		return;
	}

	new_ch = adapter->mlmeextpriv.cur_channel;
	new_bw = adapter->mlmeextpriv.cur_bwmode;
	new_offset = adapter->mlmeextpriv.cur_ch_offset;
	old_ch = grp->bound_channel;

	if (new_ch == 0 || (new_ch == old_ch && new_bw == grp->bound_bw)) {
		spin_unlock_irqrestore(&grp->lock, flags);
		return;
	}

	grp->bound_channel = new_ch;
	grp->bound_bw = new_bw;
	snap_count = grp->num_helpers;
	for (i = 0; i < snap_count; i++)
		helper_snap[i] = grp->helpers[i];

	spin_unlock_irqrestore(&grp->lock, flags);

	RTW_INFO("%s: primary channel changed %u -> %u (bw=%u), "
		 "moving helpers\n", __func__, old_ch, new_ch, new_bw);

	for (i = 0; i < snap_count; i++) {
		if (!helper_snap[i])
			continue;
		/* Skip helpers whose netdev is down -- set_channel_bwmode
		 * on a dead interface is a no-op at best, and the helper
		 * will need full re-init to recover anyway. */
		if (helper_snap[i]->pnetdev &&
		    !(helper_snap[i]->pnetdev->flags & IFF_UP)) {
			RTW_WARN("%s: helper iface_id=%d is DOWN, "
				 "skipping channel move\n",
				 __func__, helper_snap[i]->iface_id);
			continue;
		}
		set_channel_bwmode(helper_snap[i], new_ch,
				   new_offset, new_bw);
	}
}


/*
 * ============================================================
 * Non-QoS Duplicate Detection Cache
 * ============================================================
 *
 * For AMPDU/QoS traffic, the reorder window provides natural dedup.
 * For non-QoS traffic, we maintain a small ring buffer of recently
 * seen sequence numbers.
 */

static bool coop_nonqos_check_and_record(struct cooperative_rx_group *grp,
					  u16 seq_num, const u8 *ta)
{
	struct coop_nonqos_seq_cache *cache = &grp->nonqos_cache;
	int i;

	/* valid bitmask is u32 -- cache size must not exceed 32 slots */
	BUILD_BUG_ON(COOP_NONQOS_SEQ_CACHE_SZ > 32);

	for (i = 0; i < COOP_NONQOS_SEQ_CACHE_SZ; i++) {
		if ((cache->valid & BIT(i)) &&
		    cache->entries[i].seq == seq_num &&
		    _rtw_memcmp(cache->entries[i].ta, ta, ETH_ALEN) == _TRUE)
			return true;
	}

	/* Not a dup -- record it */
	cache->entries[cache->idx].seq = seq_num;
	_rtw_memcpy(cache->entries[cache->idx].ta, ta, ETH_ALEN);
	cache->valid |= BIT(cache->idx);
	cache->idx = (cache->idx + 1) % COOP_NONQOS_SEQ_CACHE_SZ;
	return false;
}


/*
 * ============================================================
 * Helper RX Hot Path -- Entry Point and Frame Injection
 * ============================================================
 */

/*
 * Pre-recv entry hook for cooperative RX helpers.
 *
 * Called from pre_recv_entry() in rtw_recv.c. Handles the complete
 * helper frame lifecycle: 802.11 header parsing, validation, and
 * submission to the primary's RX path.
 *
 * Returns RTW_RX_HANDLED if the frame was consumed (accepted or
 * rejected), in which case the caller must NOT touch the frame.
 * Returns _FAIL if this adapter is not a helper or the frame is
 * not a data frame, in which case normal processing continues.
 *
 * This function exists so that the hook in rtw_recv.c is a single
 * call, making the cooperative RX code easy to port across drivers.
 */
int rtw_coop_rx_pre_recv_entry(union recv_frame *precvframe,
			       _adapter *adapter, u8 *pbuf, u8 *pphy_status)
{
	struct rx_pkt_attrib *pattrib;
	u8 frame_type, to_fr_ds;
	int ret;

	if (!rtw_coop_rx_is_helper(adapter))
		return _FAIL;

	frame_type = GetFrameType(pbuf);
	if (frame_type != WIFI_DATA_TYPE)
		return _FAIL; /* non-data: let normal monitor path handle */

	pattrib = &precvframe->u.hdr.attrib;

	/* Query PHY status for RSSI info */
	if (pphy_status)
		rx_query_phy_status(precvframe, pphy_status);

	/* Parse 802.11 header -- address layout depends on To DS / From DS */
	to_fr_ds = get_tofr_ds(pbuf);
	pattrib->to_fr_ds = to_fr_ds;
	pattrib->seq_num = GetSequence(pbuf);
	pattrib->frag_num = GetFragNum(pbuf);
	pattrib->privacy = GetPrivacy(pbuf);

	switch (to_fr_ds) {
	case 2: /* From DS=1, To DS=0: AP->STA */
		_rtw_memcpy(pattrib->ra, GetAddr1Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->ta, get_addr2_ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->dst, GetAddr1Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->src, GetAddr3Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->bssid, get_addr2_ptr(pbuf), ETH_ALEN);
		break;
	case 1: /* From DS=0, To DS=1: STA->AP */
		_rtw_memcpy(pattrib->ra, GetAddr1Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->ta, get_addr2_ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->dst, GetAddr3Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->src, get_addr2_ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->bssid, GetAddr1Ptr(pbuf), ETH_ALEN);
		break;
	default:
		_rtw_memcpy(pattrib->ra, GetAddr1Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->ta, get_addr2_ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->dst, GetAddr1Ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->src, get_addr2_ptr(pbuf), ETH_ALEN);
		_rtw_memcpy(pattrib->bssid, GetAddr3Ptr(pbuf), ETH_ALEN);
		break;
	}

	/* Parse QoS/TID if present */
	if ((get_frame_sub_type(pbuf) & WIFI_QOS_DATA_TYPE)
	    == WIFI_QOS_DATA_TYPE) {
		u8 a4_shift = (to_fr_ds == 3) ? ETH_ALEN : 0;

		pattrib->qos = 1;
		pattrib->priority = GetPriority(
			pbuf + WLAN_HDR_A3_LEN + a4_shift);
	} else {
		pattrib->qos = 0;
		pattrib->priority = 0;
	}

	ret = rtw_coop_rx_submit_helper_frame(precvframe, adapter);
	if (ret == RTW_RX_HANDLED)
		return RTW_RX_HANDLED;

	/* Merge rejected -- free the frame back to the helper's pool */
	rtw_free_recvframe(precvframe,
			   &adapter->recvpriv.free_recv_queue);
	return RTW_RX_HANDLED; /* frame consumed either way */
}

/*
 * This is the validation and injection function called from
 * rtw_coop_rx_pre_recv_entry(). It takes a parsed recv_frame from
 * the helper, validates it, and injects it into the primary
 * adapter's RX processing.
 *
 * Duplicate suppression uses two layers for open networks:
 *   1. Reorder window -- catches in-window seq_num collisions (QoS)
 *   2. Non-QoS dedup cache -- ring buffer of recent seq numbers
 *
 * CRITICAL: Copy rx_head/data/tail/end from the HELPER's recv_frame
 * (precvframe->u.hdr.rx_*), NOT from pkt->head/data. Using SKB
 * pointers directly breaks everything because USB bulk-in header
 * padding offsets are lost.
 */
int rtw_coop_rx_submit_helper_frame(union recv_frame *precvframe,
				    _adapter *helper_adapter)
{
	struct cooperative_rx_group *grp;
	struct rx_pkt_attrib *pattrib;
	_adapter *primary;
	struct sta_priv *pstapriv;
	struct sta_info *psta;
	union recv_frame *pframe_primary;
	struct recv_priv *precvpriv_primary;

	rcu_read_lock();

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp || grp->state != COOP_STATE_ACTIVE) {
		rcu_read_unlock();
		return _FAIL;
	}

	primary = grp->primary;
	if (!primary || rtw_is_drv_stopped(primary)) {
		rcu_read_unlock();
		return _FAIL;
	}

	pattrib = &precvframe->u.hdr.attrib;

	atomic_inc(&grp->stats.helper_rx_candidates);

	/* Direction validation:
	 * STA mode: accept to_fr_ds=2 (AP->STA downlink)
	 * AP mode:  accept to_fr_ds=1 (STA->AP uplink) */
	if (pattrib->to_fr_ds == 2) {
		/* Downlink AP->STA: STA-mode cooperative RX */
	} else if (pattrib->to_fr_ds == 1) {
		/* Uplink STA->AP: AP-mode cooperative RX */
		if (!check_fwstate(&primary->mlmepriv, WIFI_AP_STATE)) {
			atomic_inc(&grp->stats.helper_rx_foreign);
			rcu_read_unlock();
			return _FAIL;
		}
	} else {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/* Validate TA/RA based on direction.
	 * STA mode: TA must be bound AP (BSSID), RA must be our MAC or bcast.
	 * AP mode:  RA must be our AP MAC (= bound_bssid). */
	if (pattrib->to_fr_ds == 2) {
		if (_rtw_memcmp(pattrib->ta, grp->bound_bssid,
				ETH_ALEN) == _FALSE) {
			atomic_inc(&grp->stats.helper_rx_foreign);
			rcu_read_unlock();
			return _FAIL;
		}
		if (!IS_MCAST(pattrib->ra) &&
		    _rtw_memcmp(pattrib->ra, adapter_mac_addr(primary),
				ETH_ALEN) == _FALSE) {
			atomic_inc(&grp->stats.helper_rx_foreign);
			rcu_read_unlock();
			return _FAIL;
		}
	} else {
		/* AP mode (to_fr_ds=1): RA = our AP MAC */
		if (_rtw_memcmp(pattrib->ra, grp->bound_bssid,
				ETH_ALEN) == _FALSE) {
			atomic_inc(&grp->stats.helper_rx_foreign);
			rcu_read_unlock();
			return _FAIL;
		}
	}

	/* CRC/ICV errors are useless */
	if (pattrib->crc_err || pattrib->icv_err) {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/*
	 * Look up the sta_info on the PRIMARY adapter -- this is the
	 * station context with the reorder windows.
	 */
	pstapriv = &primary->stapriv;
	psta = rtw_get_stainfo(pstapriv, pattrib->ta);
	if (!psta) {
		atomic_inc(&grp->stats.helper_rx_no_sta);
		rcu_read_unlock();
		return _FAIL;
	}

	/* Open network only: reject encrypted frames */
	if (pattrib->privacy) {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/*
	 * Run remaining validation checks BEFORE allocating a primary
	 * recv_frame, so that on _FAIL the caller still owns its frame.
	 */
	if (pattrib->qos) {
		u8 tid = pattrib->priority;

		if (tid > 15) {
			rcu_read_unlock();
			return _FAIL;
		}

		if (psta->recvreorder_ctrl[tid].enable) {
			u16 indicate_seq =
				psta->recvreorder_ctrl[tid].indicate_seq;
			if (indicate_seq != 0xFFFF &&
			    SN_LESS(pattrib->seq_num, indicate_seq)) {
				atomic_inc(&grp->stats.helper_rx_late);
				rcu_read_unlock();
				return _FAIL;
			}
		}
	} else {
		unsigned long flags;
		bool is_dup;

		spin_lock_irqsave(&grp->lock, flags);
		is_dup = coop_nonqos_check_and_record(grp, pattrib->seq_num,
						      pattrib->ta);
		spin_unlock_irqrestore(&grp->lock, flags);

		if (is_dup) {
			atomic_inc(&grp->stats.helper_rx_dup_dropped);
			rcu_read_unlock();
			return _FAIL;
		}
	}

	/*
	 * All checks passed -- allocate a recv_frame from the PRIMARY's
	 * pool and transfer the skb into it.  This prevents the helper's
	 * recv_frame pool from draining: the helper frame is freed back
	 * to the helper's pool immediately, and the primary frame is
	 * freed back to the primary's pool when done.
	 */
	precvpriv_primary = &primary->recvpriv;
	pframe_primary = rtw_alloc_recvframe(&precvpriv_primary->free_recv_queue);
	if (!pframe_primary) {
		atomic_inc(&grp->stats.helper_rx_pool_full);
		rcu_read_unlock();
		return _FAIL;
	}

	/* Transfer skb ownership from helper frame to primary frame */
	pframe_primary->u.hdr.pkt = precvframe->u.hdr.pkt;
	precvframe->u.hdr.pkt = NULL;  /* prevent double-free */

	/* Copy frame metadata */
	_rtw_memcpy(&pframe_primary->u.hdr.attrib,
		     &precvframe->u.hdr.attrib,
		     sizeof(struct rx_pkt_attrib));
	pframe_primary->u.hdr.len = precvframe->u.hdr.len;

	/*
	 * CRITICAL: Copy rx pointers FROM the HELPER's recv_frame, NOT
	 * from the SKB. Using pkt->head/data directly breaks because
	 * USB bulk-in header padding offsets are lost.
	 */
	pframe_primary->u.hdr.rx_head = precvframe->u.hdr.rx_head;
	pframe_primary->u.hdr.rx_data = precvframe->u.hdr.rx_data;
	pframe_primary->u.hdr.rx_tail = precvframe->u.hdr.rx_tail;
	pframe_primary->u.hdr.rx_end = precvframe->u.hdr.rx_end;

	/*
	 * Strip FCS from monitor-mode helper frames.
	 *
	 * Monitor mode intentionally keeps the 4-byte FCS in pkt_len
	 * (for radiotap), but recv_func_posthandle computes offsets
	 * from hdr.len. If the FCS is included, downstream processing
	 * reads from FCS bytes instead of valid payload.
	 */
#ifdef CONFIG_RX_PACKET_APPEND_FCS
	if (pframe_primary->u.hdr.len >= IEEE80211_FCS_LEN) {
		pframe_primary->u.hdr.len -= IEEE80211_FCS_LEN;
		pframe_primary->u.hdr.rx_tail -= IEEE80211_FCS_LEN;
	}
#endif

	/* Associate with PRIMARY adapter context */
	pframe_primary->u.hdr.adapter = primary;
	pframe_primary->u.hdr.psta = psta;

	/* Set reorder control pointer for the drain tasklet.
	 * Normally set by validate_recv() which we bypass.
	 * recv_func_posthandle() -> recv_indicatepkt_reorder()
	 * reads this to find the correct reorder window. */
	if (pattrib->qos && !IS_MCAST(pattrib->ra))
		pframe_primary->u.hdr.preorder_ctrl =
			&psta->recvreorder_ctrl[pattrib->priority];
	else
		pframe_primary->u.hdr.preorder_ctrl = NULL;

	/*
	 * Free the helper's recv_frame back to the helper's pool NOW.
	 * The skb has been transferred (pkt==NULL), so rtw_os_free_recvframe
	 * will skip the skb free.
	 */
	rtw_free_recvframe(precvframe,
			   &helper_adapter->recvpriv.free_recv_queue);

	/*
	 * Deferred processing: enqueue to pending_queue and schedule
	 * the drain tasklet. This decouples the helper's softirq
	 * from the primary's recv path, avoiding contention that
	 * caused 60-86% packet loss on the primary at high rates.
	 */
	rtw_enqueue_recvframe(pframe_primary, &grp->pending_queue);
	if (atomic_inc_return(&grp->pending_count) > coop_pending_max(grp)) {
		/* Over limit -- dequeue and drop. atomic_inc_return makes
		 * the check-and-increment atomic, preventing burst overrun. */
		pframe_primary = rtw_alloc_recvframe(&grp->pending_queue);
		atomic_dec(&grp->pending_count);
		atomic_inc(&grp->stats.helper_rx_backpressure);
		if (pframe_primary)
			rtw_free_recvframe(pframe_primary,
					   &primary->recvpriv.free_recv_queue);
		rcu_read_unlock();
		return RTW_RX_HANDLED;
	}
	atomic_inc(&grp->stats.helper_rx_deferred);
	tasklet_schedule(&grp->coop_rx_tasklet);

	rcu_read_unlock();
	return RTW_RX_HANDLED;
}


/*
 * ============================================================
 * Drain Tasklet -- Deferred Helper Frame Processing
 * ============================================================
 *
 * Processes frames enqueued by rtw_coop_rx_submit_helper_frame().
 * Runs in its own softirq context, decoupled from the helper's
 * USB recv_tasklet, eliminating contention on the primary's
 * recv path that caused packet loss at high frame rates.
 *
 * CRITICAL: ALL frames go through recv_func_posthandle(primary,
 * pframe). Do NOT bypass it with direct recv_indicatepkt_reorder
 * calls -- that causes reorder window stalls and missing
 * IV/MIC stripping.
 *
 * For open networks: pa->encrypt will be 0, so
 * recv_func_posthandle simply does defrag -> reorder -> indicate.
 */
static void _coop_rx_drain_tasklet(struct cooperative_rx_group *grp);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
void rtw_coop_rx_drain_tasklet(struct tasklet_struct *t)
{
	struct cooperative_rx_group *grp = from_tasklet(grp, t, coop_rx_tasklet);
	_coop_rx_drain_tasklet(grp);
}
#else
static void rtw_coop_rx_drain_tasklet_compat(unsigned long data)
{
	_coop_rx_drain_tasklet((struct cooperative_rx_group *)data);
}
#endif

#define COOP_RCU_BATCH 64

static void _coop_rx_drain_tasklet(struct cooperative_rx_group *grp)
{
	_adapter *primary;
	union recv_frame *pframe;
	int processed = 0;
	int ret;

	rcu_read_lock();

	primary = READ_ONCE(grp->primary);
	if (!primary || rtw_is_drv_stopped(primary) ||
	    READ_ONCE(grp->state) != COOP_STATE_ACTIVE) {
		rcu_read_unlock();
		return;
	}

	while (processed < COOP_BATCH_SIZE) {
		struct rx_pkt_attrib *pa;
		struct sta_info *psta;

		/* Dequeue from pending_queue. rtw_alloc_recvframe() is the
		 * driver's standard queue-head dequeue; despite its name
		 * (designed for free-pool allocation), it works on any
		 * _queue and is used here intentionally. */
		pframe = rtw_alloc_recvframe(&grp->pending_queue);
		if (!pframe)
			break;

		atomic_dec(&grp->pending_count);

		pa = &pframe->u.hdr.attrib;

		/* Fresh psta lookup via rtw_get_stainfo (not cached) */
		psta = rtw_get_stainfo(&primary->stapriv, pa->ta);

		/*
		 * Dedup mirroring recv_decache(): compare the frame's
		 * seq_ctrl against the primary's cached rxseq.  If they
		 * match, the primary already processed this frame.
		 */
		if (psta) {
			u16 seq_ctrl = (pa->seq_num << 4) |
				       (pa->frag_num & 0xf);
			u16 *prxseq;
			sint tid = pa->priority;

			if (tid > 15)
				tid = 0;

			if (pa->qos) {
				if (IS_MCAST(pa->ra))
					prxseq = &psta->sta_recvpriv
						  .bmc_tid_rxseq[tid];
				else
					prxseq = &psta->sta_recvpriv.rxcache
						  .tid_rxseq[tid];
			} else {
				if (IS_MCAST(pa->ra))
					prxseq = &psta->sta_recvpriv
						  .nonqos_bmc_rxseq;
				else
					prxseq = &psta->sta_recvpriv
						  .nonqos_rxseq;
			}

			if (seq_ctrl == READ_ONCE(*prxseq)) {
				atomic_inc(&grp->stats.helper_rx_dup_dropped);
				rtw_free_recvframe(pframe,
					&primary->recvpriv.free_recv_queue);
				processed++;
				continue;
			}

			/* Claim this frame -- write rxseq so primary's
			 * recv_decache sees it as consumed. */
			WRITE_ONCE(*prxseq, seq_ctrl);
		}

		/*
		 * Pre-reorder window check: if the primary's window has
		 * advanced past this seq_num, it already indicated it.
		 * Reading indicate_seq without the reorder lock is safe:
		 * it only advances monotonically, so a stale read = false
		 * negative (miss a dup), never false positive (drop valid).
		 */
		if (pa->qos && psta) {
			u8 ptid = pa->priority;

			if (ptid <= 15) {
				struct recv_reorder_ctrl *preorder =
					&psta->recvreorder_ctrl[ptid];
				u16 ind_seq =
					READ_ONCE(preorder->indicate_seq);

				if (preorder->enable &&
				    ind_seq != 0xFFFF &&
				    SN_LESS(pa->seq_num, ind_seq)) {
					atomic_inc(&grp->stats
						   .helper_rx_dup_dropped);
					rtw_free_recvframe(pframe,
						&primary->recvpriv
						.free_recv_queue);
					processed++;
					continue;
				}
			}
		}

		/*
		 * Capture values from pframe BEFORE calling recv functions,
		 * which may consume and free the frame (use-after-free).
		 */
		{
			uint _helper_pktlen = 0;
			s8 _helper_rssi;
			u8 _helper_priority;

			if (pframe->u.hdr.pkt)
				_helper_pktlen = pframe->u.hdr.pkt->len;
			_helper_rssi = pa->phy_info.recv_signal_power;
			_helper_priority = pa->priority;

			/* Bounds-check priority for recvreorder_ctrl index */
			if (_helper_priority > 15)
				_helper_priority = 0;

			/*
			 * CRITICAL: ALL frames go through recv_func_posthandle.
			 * For open networks, encrypt==0 so this simply does
			 * defrag -> reorder -> indicate. Never bypass this
			 * with direct recv_indicatepkt_reorder calls.
			 */
			ret = recv_func_posthandle(primary, pframe);

			/*
			 * After this point, pframe/pa/psta may be freed.
			 * Use only pre-captured values (_helper_*).
			 */
			if (ret == _SUCCESS || ret == RTW_RX_HANDLED) {
				s8 primary_rssi = primary->recvpriv.rssi;

				atomic_inc(&grp->stats.helper_rx_accepted);
				atomic_long_add(_helper_pktlen,
						&grp->stats.helper_rx_bytes);
				if (_helper_rssi > primary_rssi)
					atomic_inc(&grp->stats.helper_rx_rssi_better);
				else
					atomic_inc(&grp->stats.helper_rx_rssi_worse);
			} else {
				atomic_inc(&grp->stats.helper_rx_dup_dropped);
			}
		} /* end pre-capture scope */

		processed++;

		/* RCU batch breakup every COOP_RCU_BATCH frames */
		if (processed % COOP_RCU_BATCH == 0) {
			rcu_read_unlock();
			rcu_read_lock();
		}
	}

	/* If queue still has frames, reschedule */
	if (atomic_read(&grp->pending_count) > 0)
		tasklet_schedule(&grp->coop_rx_tasklet);

	rcu_read_unlock();
}


/*
 * ============================================================
 * sysfs Interface
 * ============================================================
 *
 * Provides /sys/class/net/wlanX/coop_rx/ directory with:
 *   enabled       - read/write 0/1
 *   role          - read: "primary", "helper", "none"
 *   stats         - read: statistics counters
 *   info          - read: machine-parseable setup summary (key=value)
 *   pair          - write: interface name to pair as helper
 *   unpair        - write: interface name to unpair
 *   auto_pair     - write: 1 to set primary and auto-discover helpers
 *   bind          - write: 1 to bind session (after association)
 *   reset_stats   - write: 1 to zero all counters
 *   drop_primary  - read/write: debug flag to drop primary RX data
 */

static ssize_t coop_rx_show_enabled(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 grp ? (grp->state >= COOP_STATE_IDLE ? 1 : 0) : 0);
}

static ssize_t coop_rx_store_enabled(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	_adapter *adapter = rtw_netdev_priv(ndev);
	struct cooperative_rx_group *grp;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return -ENODEV;

	if (val == 0) {
		rtw_coop_rx_unbind_session();
		RTW_INFO("coop_rx: disabled via sysfs\n");
	} else if (val == 1 && grp->primary == adapter &&
		   grp->state == COOP_STATE_IDLE) {
		/* Re-enable: rebind session if primary is associated */
		int ret = rtw_coop_rx_bind_session(adapter);

		if (ret)
			RTW_WARN("coop_rx: re-enable failed (%d)\n", ret);
		else
			RTW_INFO("coop_rx: re-enabled via sysfs\n");
	}

	return count;
}

static ssize_t coop_rx_show_role(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct net_device *ndev = to_net_dev(dev);
	_adapter *adapter = rtw_netdev_priv(ndev);
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);
	const char *role = "none";

	if (grp) {
		if (grp->primary == adapter)
			role = "primary";
		else if (rtw_coop_rx_is_helper(adapter))
			role = "helper";
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", role);
}

/*
 * Common stats formatter -- shared between sysfs and debugfs.
 * Returns number of bytes written.
 */
static int coop_rx_format_stats(char *buf, size_t size,
				struct cooperative_rx_group *grp)
{
	return scnprintf(buf, size,
		"state: %d (%s)\n"
		"primary: %s\n"
		"num_helpers: %d\n"
		"bound_bssid: "MAC_FMT"\n"
		"bound_channel: %u\n"
		"bound_bw: %u\n"
		"helper_rx_candidates: %d\n"
		"helper_rx_accepted: %d\n"
		"helper_rx_dup_dropped: %d\n"
		"helper_rx_pool_full: %d\n"
		"helper_rx_foreign: %d\n"
		"helper_rx_late: %d\n"
		"helper_rx_no_sta: %d\n"
		"helper_rx_deferred: %d\n"
		"helper_rx_backpressure: %d\n"
		"helper_rx_rssi_better: %d\n"
		"helper_rx_rssi_worse: %d\n"
		"pending_count: %d\n"
		"fallback_events: %d\n"
		"pair_events: %d\n"
		"unpair_events: %d\n"
		"helper_rx_bytes: %ld\n",
		grp->state,
		grp->state == COOP_STATE_DISABLED ? "DISABLED" :
		grp->state == COOP_STATE_IDLE ? "IDLE" :
		grp->state == COOP_STATE_BINDING ? "BINDING" :
		grp->state == COOP_STATE_ACTIVE ? "ACTIVE" : "?",
		grp->primary ? "yes" : "no",
		grp->num_helpers,
		MAC_ARG(grp->bound_bssid),
		grp->bound_channel, grp->bound_bw,
		atomic_read(&grp->stats.helper_rx_candidates),
		atomic_read(&grp->stats.helper_rx_accepted),
		atomic_read(&grp->stats.helper_rx_dup_dropped),
		atomic_read(&grp->stats.helper_rx_pool_full),
		atomic_read(&grp->stats.helper_rx_foreign),
		atomic_read(&grp->stats.helper_rx_late),
		atomic_read(&grp->stats.helper_rx_no_sta),
		atomic_read(&grp->stats.helper_rx_deferred),
		atomic_read(&grp->stats.helper_rx_backpressure),
		atomic_read(&grp->stats.helper_rx_rssi_better),
		atomic_read(&grp->stats.helper_rx_rssi_worse),
		atomic_read(&grp->pending_count),
		atomic_read(&grp->stats.fallback_events),
		atomic_read(&grp->stats.pair_events),
		atomic_read(&grp->stats.unpair_events),
		atomic_long_read(&grp->stats.helper_rx_bytes));
}

static ssize_t coop_rx_show_stats(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);

	if (!grp)
		return scnprintf(buf, PAGE_SIZE, "disabled\n");

	return coop_rx_format_stats(buf, PAGE_SIZE, grp);
}

static ssize_t coop_rx_store_pair(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	_adapter *adapter = rtw_netdev_priv(ndev);
	struct net_device *helper_ndev;
	_adapter *helper_adapter;
	char ifname[IFNAMSIZ];
	int ret;

	if (sscanf(buf, "%15s", ifname) != 1)
		return -EINVAL;

	/* First, ensure this adapter is set as primary */
	ret = rtw_coop_rx_set_primary(adapter);
	if (ret)
		return ret;

	/* Find the helper interface by name (namespace-aware) */
	helper_ndev = rtw_get_same_net_ndev_by_name(ndev, ifname);
	if (!helper_ndev) {
		RTW_WARN("coop_rx: helper interface '%s' not found\n", ifname);
		return -ENODEV;
	}

	/* Verify the interface belongs to this driver */
	if (helper_ndev->netdev_ops != &rtw_netdev_ops) {
		RTW_WARN("coop_rx: '%s' is not an RTW interface\n", ifname);
		dev_put(helper_ndev);
		return -EINVAL;
	}

	helper_adapter = rtw_netdev_priv(helper_ndev);
	if (!helper_adapter) {
		dev_put(helper_ndev);
		return -EINVAL;
	}

	ret = rtw_coop_rx_add_helper(helper_adapter);
	dev_put(helper_ndev);

	if (ret)
		return ret;

	return count;
}

static ssize_t coop_rx_store_unpair(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_device *helper_ndev;
	_adapter *helper_adapter;
	char ifname[IFNAMSIZ];
	int ret;

	if (sscanf(buf, "%15s", ifname) != 1)
		return -EINVAL;

	helper_ndev = rtw_get_same_net_ndev_by_name(ndev, ifname);
	if (!helper_ndev)
		return -ENODEV;

	/* Verify the interface belongs to this driver */
	if (helper_ndev->netdev_ops != &rtw_netdev_ops) {
		dev_put(helper_ndev);
		return -EINVAL;
	}

	helper_adapter = rtw_netdev_priv(helper_ndev);
	ret = rtw_coop_rx_remove_helper(helper_adapter);
	dev_put(helper_ndev);

	return ret ? ret : count;
}

static ssize_t coop_rx_store_bind(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	_adapter *adapter = rtw_netdev_priv(ndev);
	int val, ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val == 1) {
		ret = rtw_coop_rx_bind_session(adapter);
		if (ret)
			return ret;
	} else {
		rtw_coop_rx_unbind_session();
	}

	return count;
}

static ssize_t coop_rx_store_reset_stats(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);

	if (!grp)
		return -ENODEV;

	coop_rx_stats_reset(&grp->stats);
	RTW_INFO("coop_rx: stats reset\n");
	return count;
}

static ssize_t coop_rx_show_drop_primary(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 READ_ONCE(rtw_coop_rx_drop_primary));
}

static ssize_t coop_rx_store_drop_primary(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	WRITE_ONCE(rtw_coop_rx_drop_primary, val ? 1 : 0);
	RTW_INFO("coop_rx: drop_primary_rx = %d\n", val ? 1 : 0);
	return count;
}

static ssize_t coop_rx_store_auto_pair(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	_adapter *adapter = rtw_netdev_priv(ndev);
	int val, ret;

	if (kstrtoint(buf, 10, &val) || val != 1)
		return -EINVAL;

	/* Set this adapter as primary, then discover helpers */
	ret = rtw_coop_rx_set_primary(adapter);
	if (ret)
		return ret;

	rtw_coop_rx_auto_discover_helpers(adapter);
	return count;
}

/*
 * coop_rx_info -- machine-parseable cooperative setup summary.
 * Reports per-interface details using driver-internal data so
 * userspace doesn't need to call iw (which reports stale channel
 * info for monitor-mode helpers).
 *
 * Format: key=value lines, one per field.
 */
static ssize_t coop_rx_show_info(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);
	int len = 0;
	int i;

	if (!grp) {
		return scnprintf(buf, PAGE_SIZE, "state=DISABLED\n");
	}

	/* Group-level info */
	len += scnprintf(buf + len, PAGE_SIZE - len,
		"state=%d\n"
		"state_name=%s\n"
		"bssid="MAC_FMT"\n"
		"channel=%u\n"
		"bw=%u\n"
		"num_helpers=%d\n"
		"drop_primary=%d\n",
		grp->state,
		grp->state == COOP_STATE_ACTIVE ? "ACTIVE" :
		grp->state == COOP_STATE_BINDING ? "BINDING" :
		grp->state == COOP_STATE_IDLE ? "IDLE" : "DISABLED",
		MAC_ARG(grp->bound_bssid),
		grp->bound_channel,
		grp->bound_bw,
		grp->num_helpers,
		READ_ONCE(rtw_coop_rx_drop_primary));

	/* Primary interface info */
	if (grp->primary) {
		_adapter *pri = grp->primary;
		struct mlme_priv *pmlmepriv = &pri->mlmepriv;
		struct mlme_ext_priv *pmlmeext = &pri->mlmeextpriv;
		struct recv_priv *precvpriv = &pri->recvpriv;
		WLAN_BSSID_EX *cur = &pmlmepriv->cur_network.network;

		len += scnprintf(buf + len, PAGE_SIZE - len,
			"primary_iface=%s\n"
			"primary_mode=%s\n"
			"primary_channel=%u\n"
			"primary_rssi=%d\n"
			"primary_signal=%u\n",
			pri->pnetdev ? pri->pnetdev->name : "?",
			MLME_IS_AP(pri) ? "AP" :
			MLME_IS_STA(pri) ? "STA" : "?",
			pmlmeext->cur_channel,
			precvpriv->rssi,
			precvpriv->signal_strength);

		if (check_fwstate(pmlmepriv, WIFI_ASOC_STATE) &&
		    cur->Ssid.SsidLength > 0 &&
		    cur->Ssid.SsidLength <= 32) {
			/* Safe SSID output -- replace non-printable chars */
			char ssid_buf[33];
			u32 slen = cur->Ssid.SsidLength;
			u32 j;

			memcpy(ssid_buf, cur->Ssid.Ssid, slen);
			ssid_buf[slen] = '\0';
			for (j = 0; j < slen; j++) {
				if (ssid_buf[j] < 0x20 || ssid_buf[j] > 0x7e)
					ssid_buf[j] = '?';
			}
			len += scnprintf(buf + len, PAGE_SIZE - len,
				"primary_ssid=%s\n"
				"primary_connected=1\n",
				ssid_buf);
		} else {
			len += scnprintf(buf + len, PAGE_SIZE - len,
				"primary_ssid=\n"
				"primary_connected=0\n");
		}
	}

	/* Helper interface(s) info */
	for (i = 0; i < grp->num_helpers && i < COOP_MAX_HELPERS; i++) {
		_adapter *hlp = grp->helpers[i];

		if (!hlp)
			continue;

		/* Use dvobj->oper_channel for actual HW channel:
		 * mlmeextpriv.cur_channel isn't updated for monitor-mode
		 * adapters, but set_channel_bwmode() updates oper_channel
		 * via rtw_set_oper_ch(). */
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"helper%d_iface=%s\n"
			"helper%d_channel=%u\n"
			"helper%d_rssi=%d\n"
			"helper%d_signal=%u\n",
			i, hlp->pnetdev ? hlp->pnetdev->name : "?",
			i, adapter_to_dvobj(hlp)->oper_channel,
			i, hlp->recvpriv.rssi,
			i, hlp->recvpriv.signal_strength);
	}

	return len;
}

static DEVICE_ATTR(coop_rx_enabled, 0600,
		   coop_rx_show_enabled, coop_rx_store_enabled);
static DEVICE_ATTR(coop_rx_role, 0444, coop_rx_show_role, NULL);
static DEVICE_ATTR(coop_rx_stats, 0444, coop_rx_show_stats, NULL);
static DEVICE_ATTR(coop_rx_info, 0444, coop_rx_show_info, NULL);
static DEVICE_ATTR(coop_rx_pair, 0200, NULL, coop_rx_store_pair);
static DEVICE_ATTR(coop_rx_unpair, 0200, NULL, coop_rx_store_unpair);
static DEVICE_ATTR(coop_rx_bind, 0200, NULL, coop_rx_store_bind);
static DEVICE_ATTR(coop_rx_reset_stats, 0200, NULL, coop_rx_store_reset_stats);
static DEVICE_ATTR(coop_rx_drop_primary, 0600,
		   coop_rx_show_drop_primary, coop_rx_store_drop_primary);
static DEVICE_ATTR(coop_rx_auto_pair, 0200, NULL, coop_rx_store_auto_pair);

static struct attribute *coop_rx_attrs[] = {
	&dev_attr_coop_rx_enabled.attr,
	&dev_attr_coop_rx_role.attr,
	&dev_attr_coop_rx_stats.attr,
	&dev_attr_coop_rx_info.attr,
	&dev_attr_coop_rx_pair.attr,
	&dev_attr_coop_rx_unpair.attr,
	&dev_attr_coop_rx_bind.attr,
	&dev_attr_coop_rx_auto_pair.attr,
	&dev_attr_coop_rx_reset_stats.attr,
	&dev_attr_coop_rx_drop_primary.attr,
	NULL,
};

static const struct attribute_group coop_rx_attr_group = {
	.name = "coop_rx",
	.attrs = coop_rx_attrs,
};

int rtw_coop_rx_sysfs_init(struct net_device *ndev)
{
	if (!rtw_coop_rx_enabled())
		return 0;
	return sysfs_create_group(&ndev->dev.kobj, &coop_rx_attr_group);
}

void rtw_coop_rx_sysfs_deinit(struct net_device *ndev)
{
	if (!rtw_coop_rx_enabled())
		return;
	sysfs_remove_group(&ndev->dev.kobj, &coop_rx_attr_group);
}


/*
 * ============================================================
 * debugfs Interface
 * ============================================================
 */

#ifdef CONFIG_DEBUG_FS

static struct dentry *coop_debugfs_dir;

static int coop_debugfs_stats_show(struct seq_file *s, void *data)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);
	char buf[1024];
	int len;

	if (!grp) {
		seq_puts(s, "cooperative RX: not initialized\n");
		return 0;
	}

	seq_puts(s, "=== Cooperative RX Diversity Stats ===\n");
	len = coop_rx_format_stats(buf, sizeof(buf), grp);
	seq_write(s, buf, len);

	return 0;
}

static int coop_debugfs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, coop_debugfs_stats_show, inode->i_private);
}

static const struct file_operations coop_debugfs_stats_fops = {
	.open = coop_debugfs_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int rtw_coop_rx_debugfs_init(void)
{
	coop_debugfs_dir = debugfs_create_dir("rtw_coop_rx", NULL);
	if (IS_ERR_OR_NULL(coop_debugfs_dir)) {
		RTW_WARN("coop_rx: failed to create debugfs directory\n");
		coop_debugfs_dir = NULL;
		return -ENOMEM;
	}

	debugfs_create_file("stats", 0444, coop_debugfs_dir,
			    NULL, &coop_debugfs_stats_fops);

	return 0;
}

void rtw_coop_rx_debugfs_deinit(void)
{
	if (coop_debugfs_dir) {
		debugfs_remove_recursive(coop_debugfs_dir);
		coop_debugfs_dir = NULL;
	}
}

#else /* !CONFIG_DEBUG_FS */

int rtw_coop_rx_debugfs_init(void) { return 0; }
void rtw_coop_rx_debugfs_deinit(void) { }

#endif /* CONFIG_DEBUG_FS */
