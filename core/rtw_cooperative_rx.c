/******************************************************************************
 *
 * Copyright(c) 2024 Realtek Corporation.
 *
 * Cooperative RX Diversity Mode — Core Implementation
 *
 * Enables a helper USB adapter to contribute received frames to a primary
 * adapter's RX path, improving robustness under fading/interference.
 *
 * Architecture:
 *   - Primary adapter operates normally in STA mode
 *   - Helper adapter(s) are slaved to the same channel/BSSID
 *   - Helper RX frames are validated and injected into primary's
 *     AMPDU reorder window, which provides natural duplicate suppression
 *   - One coherent RX stream is delivered to the network stack
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 *****************************************************************************/

#define pr_fmt(fmt) "rtw_coop_rx: " fmt

#include <drv_types.h>
#include <rtw_recv.h>
#include <rtw_cooperative_rx.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

/* Module parameter — 0=disabled (default), 1=enabled */
int rtw_cooperative_rx = 0;

/* Global cooperative group singleton */
struct cooperative_rx_group *rtw_coop_rx_group = NULL;

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
	memset(&grp->stats, 0, sizeof(grp->stats));

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

	spin_lock_irqsave(&grp->lock, flags);
	grp->state = COOP_STATE_DISABLED;
	grp->primary = NULL;
	grp->num_helpers = 0;
	spin_unlock_irqrestore(&grp->lock, flags);

	/* Ensure no readers are in the hot path */
	synchronize_rcu();

	WRITE_ONCE(rtw_coop_rx_group, NULL);
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

	if (grp->state == COOP_STATE_IDLE || grp->state == COOP_STATE_DISABLED)
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
		RTW_INFO("%s: primary adapter removed, tearing down "
			 "cooperative group\n", __func__);
		grp->state = COOP_STATE_TEARDOWN;
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
 * Bind cooperative session to primary's current BSS.
 * Called after primary successfully associates.
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

	if (!check_fwstate(pmlmepriv, WIFI_STATION_STATE) ||
	    !check_fwstate(pmlmepriv, WIFI_ASOC_STATE)) {
		RTW_WARN("%s: primary not associated in STA mode\n", __func__);
		return -ENOTCONN;
	}

	spin_lock_irqsave(&grp->lock, flags);

	/* Capture BSS context */
	_rtw_memcpy(grp->bound_bssid,
		     cur_network->network.MacAddress, ETH_ALEN);
	_rtw_memcpy(grp->bound_ap_mac,
		     cur_network->network.MacAddress, ETH_ALEN);
	grp->bound_channel = primary->mlmeextpriv.cur_channel;
	grp->bound_bw = primary->mlmeextpriv.cur_bwmode;

	/* Reset non-QoS dedup cache */
	memset(&grp->nonqos_cache, 0, sizeof(grp->nonqos_cache));

	if (grp->num_helpers > 0)
		grp->state = COOP_STATE_ACTIVE;

	/* Enable monitor mode on all helpers and park on bound channel.
	 * Must drop spinlock before calling rtw_setopmode_cmd (sleeps). */
	for (i = 0; i < grp->num_helpers; i++) {
		if (grp->helpers[i]) {
			_adapter *helper = grp->helpers[i];
			u8 ch = grp->bound_channel;

			spin_unlock_irqrestore(&grp->lock, flags);
			rtw_coop_rx_enable_helper_monitor(helper, ch);
			spin_lock_irqsave(&grp->lock, flags);
		}
	}

	spin_unlock_irqrestore(&grp->lock, flags);

	RTW_INFO("%s: session bound to BSSID="MAC_FMT" ch=%u bw=%u "
		 "(helpers=%d, state=%s)\n",
		 __func__, MAC_ARG(grp->bound_bssid),
		 grp->bound_channel, grp->bound_bw,
		 grp->num_helpers,
		 grp->state == COOP_STATE_ACTIVE ? "ACTIVE" : "BINDING");
	return 0;
}

/*
 * Enable monitor mode on a helper adapter via driver internals.
 * iw/cfg80211 set type monitor does NOT call the driver's change_iface
 * callback when the interface is down, so WIFI_MONITOR_STATE is never
 * set and the RCR stays closed.  This function uses the driver's own
 * APIs to properly enter monitor mode and park on the bound channel.
 */
int rtw_coop_rx_enable_helper_monitor(_adapter *helper, u8 channel)
{
	struct net_device *ndev = helper->pnetdev;

	/* Set netdev type for radiotap headers */
	ndev->type = ARPHRD_IEEE80211_RADIOTAP;

	/* Update cfg80211 wdev type so iw/NM report "monitor" correctly */
	if (helper->rtw_wdev)
		helper->rtw_wdev->iftype = NL80211_IFTYPE_MONITOR;

	/* Set driver internal mode — configures WIFI_MONITOR_STATE,
	 * sets RCR to promiscuous, opens RX filter maps */
	rtw_set_802_11_infrastructure_mode(helper, Ndis802_11Monitor, 0);
	rtw_setopmode_cmd(helper, Ndis802_11Monitor, RTW_CMDF_WAIT_ACK);

	/* Park on the bound channel */
	set_channel_bwmode(helper, channel,
			   HAL_PRIME_CHNL_OFFSET_DONT_CARE, CHANNEL_WIDTH_20);

	RTW_INFO("%s: helper iface_id=%d set to monitor mode ch=%u\n",
		 __func__, helper->iface_id, channel);
	return 0;
}

/*
 * Called after the primary adapter's channel changes (CSA, roam, etc.).
 * If the new channel differs from the bound channel, move all helpers
 * to the new channel so they continue receiving frames.
 */
void rtw_coop_rx_notify_channel_switch(_adapter *adapter)
{
	struct cooperative_rx_group *grp;
	u8 new_ch;
	int i;

	if (!rtw_coop_rx_enabled())
		return;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp || grp->state != COOP_STATE_ACTIVE)
		return;

	if (grp->primary != adapter)
		return;

	new_ch = adapter->mlmeextpriv.cur_channel;
	if (new_ch == 0 || new_ch == grp->bound_channel)
		return;

	RTW_INFO("%s: primary channel changed %u -> %u, moving helpers\n",
		 __func__, grp->bound_channel, new_ch);

	grp->bound_channel = new_ch;

	for (i = 0; i < grp->num_helpers; i++) {
		if (grp->helpers[i])
			set_channel_bwmode(grp->helpers[i], new_ch,
					   HAL_PRIME_CHNL_OFFSET_DONT_CARE,
					   CHANNEL_WIDTH_20);
	}
}

void rtw_coop_rx_unbind_session(void)
{
	struct cooperative_rx_group *grp;
	unsigned long flags;

	grp = READ_ONCE(rtw_coop_rx_group);
	if (!grp)
		return;

	spin_lock_irqsave(&grp->lock, flags);
	if (grp->state == COOP_STATE_ACTIVE ||
	    grp->state == COOP_STATE_BINDING) {
		grp->state = grp->primary ? COOP_STATE_IDLE : COOP_STATE_DISABLED;
		memset(grp->bound_bssid, 0, ETH_ALEN);
		memset(grp->bound_ap_mac, 0, ETH_ALEN);
		grp->bound_channel = 0;
		RTW_INFO("%s: session unbound\n", __func__);
	}
	spin_unlock_irqrestore(&grp->lock, flags);
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

static bool coop_nonqos_is_dup(struct cooperative_rx_group *grp, u16 seq_num)
{
	struct coop_nonqos_seq_cache *cache = &grp->nonqos_cache;
	int i;

	for (i = 0; i < COOP_NONQOS_SEQ_CACHE_SZ; i++) {
		if (cache->seqs[i] == seq_num)
			return true;
	}

	/* Not a dup — record it */
	cache->seqs[cache->idx] = seq_num;
	cache->idx = (cache->idx + 1) % COOP_NONQOS_SEQ_CACHE_SZ;
	return false;
}

/*
 * ============================================================
 * Helper RX Hot Path — Frame Injection
 * ============================================================
 *
 * This is the critical function called from the helper adapter's
 * RX tasklet path. It takes a parsed recv_frame from the helper,
 * validates it, and injects it into the primary adapter's RX
 * processing.
 *
 * The key insight is that the AMPDU reorder window
 * (recv_indicatepkt_reorder → enqueue_reorder_recvframe) provides
 * natural duplicate suppression: inserting a frame with a sequence
 * number that already has a slot occupied will simply be discarded.
 *
 * For non-QoS traffic, we use the nonqos_seq_cache above.
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
	int ret = _FAIL;

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

	/* Validate: frame must be from our bound BSSID */
	if (_rtw_memcmp(pattrib->bssid, grp->bound_bssid, ETH_ALEN) == _FALSE) {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/* Validate: TA must match bound AP */
	if (_rtw_memcmp(pattrib->ta, grp->bound_ap_mac, ETH_ALEN) == _FALSE) {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/* Validate: RA must match primary's MAC (unicast) or be broadcast */
	if (!IS_MCAST(pattrib->ra) &&
	    _rtw_memcmp(pattrib->ra, adapter_mac_addr(primary),
			ETH_ALEN) == _FALSE) {
		atomic_inc(&grp->stats.helper_rx_foreign);
		rcu_read_unlock();
		return _FAIL;
	}

	/* CRC/ICV errors are useless */
	if (pattrib->crc_err || pattrib->icv_err) {
		atomic_inc(&grp->stats.helper_rx_crypto_err);
		rcu_read_unlock();
		return _FAIL;
	}

	/*
	 * Look up the sta_info on the PRIMARY adapter — this is the
	 * station context with the reorder windows and crypto state.
	 */
	pstapriv = &primary->stapriv;
	psta = rtw_get_stainfo(pstapriv, pattrib->ta);
	if (!psta) {
		atomic_inc(&grp->stats.helper_rx_no_sta);
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

		if (pattrib->encrypt && !pattrib->bdecrypted) {
			struct security_priv *psec = &primary->securitypriv;

			if (psec->dot11PrivacyAlgrthm != _NO_PRIVACY_) {
				atomic_inc(&grp->stats.helper_rx_crypto_err);
				rcu_read_unlock();
				return _FAIL;
			}
		}
	} else {
		unsigned long flags;
		bool is_dup;

		spin_lock_irqsave(&grp->lock, flags);
		is_dup = coop_nonqos_is_dup(grp, pattrib->seq_num);
		spin_unlock_irqrestore(&grp->lock, flags);

		if (is_dup) {
			atomic_inc(&grp->stats.helper_rx_dup_dropped);
			rcu_read_unlock();
			return _FAIL;
		}

		if (pattrib->encrypt && !pattrib->bdecrypted) {
			atomic_inc(&grp->stats.helper_rx_crypto_err);
			rcu_read_unlock();
			return _FAIL;
		}
	}

	/*
	 * All checks passed — allocate a recv_frame from the PRIMARY's
	 * pool and transfer the skb into it.  This prevents the helper's
	 * recv_frame pool from draining: the helper frame is freed back
	 * to the helper's pool immediately, and the primary frame is
	 * freed back to the primary's pool when done.
	 */
	precvpriv_primary = &primary->recvpriv;
	pframe_primary = rtw_alloc_recvframe(&precvpriv_primary->free_recv_queue);
	if (!pframe_primary) {
		atomic_inc(&grp->stats.helper_rx_dup_dropped);
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
	pframe_primary->u.hdr.rx_head = precvframe->u.hdr.rx_head;
	pframe_primary->u.hdr.rx_data = precvframe->u.hdr.rx_data;
	pframe_primary->u.hdr.rx_tail = precvframe->u.hdr.rx_tail;
	pframe_primary->u.hdr.rx_end = precvframe->u.hdr.rx_end;

	/* Associate with PRIMARY adapter context */
	pframe_primary->u.hdr.adapter = primary;
	pframe_primary->u.hdr.psta = psta;

	/*
	 * Free the helper's recv_frame back to the helper's pool NOW.
	 * The skb has been transferred (pkt==NULL), so rtw_os_free_recvframe
	 * will skip the skb free.
	 */
	rtw_free_recvframe(precvframe,
			   &helper_adapter->recvpriv.free_recv_queue);

	/*
	 * Submit the primary frame to the appropriate processing path.
	 * From here on, the primary frame is owned by the reorder engine
	 * or the stack — we must not touch it after submission.
	 */
	if (pframe_primary->u.hdr.attrib.qos) {
		u8 tid = pframe_primary->u.hdr.attrib.priority;

		pframe_primary->u.hdr.preorder_ctrl =
			&psta->recvreorder_ctrl[tid];

		ret = recv_indicatepkt_reorder(primary, pframe_primary);
		if (ret == RTW_RX_HANDLED || ret == _SUCCESS) {
			atomic_inc(&grp->stats.helper_rx_accepted);
			ret = RTW_RX_HANDLED;
		} else {
			atomic_inc(&grp->stats.helper_rx_dup_dropped);
		}
	} else {
		recv_process_mpdu(primary, pframe_primary);
		atomic_inc(&grp->stats.helper_rx_accepted);
		ret = RTW_RX_HANDLED;
	}

	rcu_read_unlock();
	return ret;
}

/*
 * ============================================================
 * sysfs Interface
 * ============================================================
 *
 * Provides /sys/class/net/wlanX/coop_rx/ directory with:
 *   enabled    - read/write 0/1
 *   role       - read: "primary", "helper", "none"
 *   stats      - read: statistics counters
 *   pair       - write: interface name to pair as helper
 *   unpair     - write: interface name to unpair
 *   bind       - write: 1 to bind session (after association)
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
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val == 0 && rtw_coop_rx_group) {
		rtw_coop_rx_unbind_session();
		RTW_INFO("coop_rx: disabled via sysfs\n");
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

static ssize_t coop_rx_show_stats(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cooperative_rx_group *grp = READ_ONCE(rtw_coop_rx_group);
	int len = 0;

	if (!grp)
		return scnprintf(buf, PAGE_SIZE, "disabled\n");

	len += scnprintf(buf + len, PAGE_SIZE - len,
		"state: %d\n"
		"num_helpers: %d\n"
		"bound_bssid: "MAC_FMT"\n"
		"bound_channel: %u\n"
		"helper_rx_candidates: %d\n"
		"helper_rx_accepted: %d\n"
		"helper_rx_dup_dropped: %d\n"
		"helper_rx_foreign: %d\n"
		"helper_rx_crypto_err: %d\n"
		"helper_rx_late: %d\n"
		"helper_rx_no_sta: %d\n"
		"fallback_events: %d\n"
		"pair_events: %d\n"
		"unpair_events: %d\n",
		grp->state,
		grp->num_helpers,
		MAC_ARG(grp->bound_bssid),
		grp->bound_channel,
		atomic_read(&grp->stats.helper_rx_candidates),
		atomic_read(&grp->stats.helper_rx_accepted),
		atomic_read(&grp->stats.helper_rx_dup_dropped),
		atomic_read(&grp->stats.helper_rx_foreign),
		atomic_read(&grp->stats.helper_rx_crypto_err),
		atomic_read(&grp->stats.helper_rx_late),
		atomic_read(&grp->stats.helper_rx_no_sta),
		atomic_read(&grp->stats.fallback_events),
		atomic_read(&grp->stats.pair_events),
		atomic_read(&grp->stats.unpair_events));

	return len;
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

	/* Find the helper interface by name */
	helper_ndev = dev_get_by_name(&init_net, ifname);
	if (!helper_ndev) {
		RTW_WARN("coop_rx: helper interface '%s' not found\n", ifname);
		return -ENODEV;
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
	struct net_device *helper_ndev;
	_adapter *helper_adapter;
	char ifname[IFNAMSIZ];
	int ret;

	if (sscanf(buf, "%15s", ifname) != 1)
		return -EINVAL;

	helper_ndev = dev_get_by_name(&init_net, ifname);
	if (!helper_ndev)
		return -ENODEV;

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

static DEVICE_ATTR(coop_rx_enabled, 0644,
		   coop_rx_show_enabled, coop_rx_store_enabled);
static DEVICE_ATTR(coop_rx_role, 0444, coop_rx_show_role, NULL);
static DEVICE_ATTR(coop_rx_stats, 0444, coop_rx_show_stats, NULL);
static DEVICE_ATTR(coop_rx_pair, 0200, NULL, coop_rx_store_pair);
static DEVICE_ATTR(coop_rx_unpair, 0200, NULL, coop_rx_store_unpair);
static DEVICE_ATTR(coop_rx_bind, 0200, NULL, coop_rx_store_bind);

static struct attribute *coop_rx_attrs[] = {
	&dev_attr_coop_rx_enabled.attr,
	&dev_attr_coop_rx_role.attr,
	&dev_attr_coop_rx_stats.attr,
	&dev_attr_coop_rx_pair.attr,
	&dev_attr_coop_rx_unpair.attr,
	&dev_attr_coop_rx_bind.attr,
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

	if (!grp) {
		seq_puts(s, "cooperative RX: not initialized\n");
		return 0;
	}

	seq_printf(s, "=== Cooperative RX Diversity Stats ===\n");
	seq_printf(s, "state:                 %d (%s)\n", grp->state,
		   grp->state == COOP_STATE_DISABLED ? "DISABLED" :
		   grp->state == COOP_STATE_IDLE ? "IDLE" :
		   grp->state == COOP_STATE_BINDING ? "BINDING" :
		   grp->state == COOP_STATE_ACTIVE ? "ACTIVE" :
		   grp->state == COOP_STATE_TEARDOWN ? "TEARDOWN" : "?");
	seq_printf(s, "primary:               %s\n",
		   grp->primary ? "yes" : "no");
	seq_printf(s, "num_helpers:           %d\n", grp->num_helpers);
	seq_printf(s, "bound_bssid:           "MAC_FMT"\n",
		   MAC_ARG(grp->bound_bssid));
	seq_printf(s, "bound_channel:         %u\n", grp->bound_channel);
	seq_printf(s, "bound_bw:              %u\n", grp->bound_bw);
	seq_printf(s, "\n--- Counters ---\n");
	seq_printf(s, "helper_rx_candidates:  %d\n",
		   atomic_read(&grp->stats.helper_rx_candidates));
	seq_printf(s, "helper_rx_accepted:    %d\n",
		   atomic_read(&grp->stats.helper_rx_accepted));
	seq_printf(s, "helper_rx_dup_dropped: %d\n",
		   atomic_read(&grp->stats.helper_rx_dup_dropped));
	seq_printf(s, "helper_rx_foreign:     %d\n",
		   atomic_read(&grp->stats.helper_rx_foreign));
	seq_printf(s, "helper_rx_crypto_err:  %d\n",
		   atomic_read(&grp->stats.helper_rx_crypto_err));
	seq_printf(s, "helper_rx_late:        %d\n",
		   atomic_read(&grp->stats.helper_rx_late));
	seq_printf(s, "helper_rx_no_sta:      %d\n",
		   atomic_read(&grp->stats.helper_rx_no_sta));
	seq_printf(s, "fallback_events:       %d\n",
		   atomic_read(&grp->stats.fallback_events));
	seq_printf(s, "pair_events:           %d\n",
		   atomic_read(&grp->stats.pair_events));
	seq_printf(s, "unpair_events:         %d\n",
		   atomic_read(&grp->stats.unpair_events));

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
