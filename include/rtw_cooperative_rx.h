/******************************************************************************
 *
 * Copyright(c) 2024 Realtek Corporation.
 *
 * Cooperative RX Diversity Mode
 *
 * Allows a helper USB adapter (same chipset) to contribute received frames
 * that the primary adapter missed, improving RX robustness under fading,
 * interference, or multipath conditions.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 *****************************************************************************/
#ifndef __RTW_COOPERATIVE_RX_H__
#define __RTW_COOPERATIVE_RX_H__

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/if_ether.h>
#include <linux/compiler.h>

#ifdef CONFIG_COOP_RX_KERNEL_CRYPTO
#include <crypto/aead.h>
#include <linux/scatterlist.h>
#endif

/* Forward declarations — full definitions come from drv_types.h */
struct _ADAPTER;
typedef struct _ADAPTER _adapter;
struct dvobj_priv;
struct net_device;
union recv_frame;
struct dentry;

#define COOP_MAX_HELPERS		4
#define COOP_NONQOS_SEQ_CACHE_SZ	32

enum coop_rx_state {
	COOP_STATE_DISABLED = 0,
	COOP_STATE_IDLE,
	COOP_STATE_BINDING,
	COOP_STATE_ACTIVE,
};

/* Statistics counters for observability */
struct coop_rx_stats {
	atomic_t helper_rx_candidates;	/* frames considered from helper */
	atomic_t helper_rx_accepted;	/* frames injected into primary */
	atomic_t helper_rx_dup_dropped;	/* duplicates caught at merge */
	atomic_t helper_rx_pool_full;	/* primary recv_frame pool exhausted */
	atomic_t helper_rx_foreign;	/* wrong BSSID/TA, rejected */
	atomic_t helper_rx_crypto_err;	/* decryption failures */
	atomic_t helper_rx_late;	/* too late for reorder window */
	atomic_t helper_rx_no_sta;	/* sta_info not found */
	atomic_t helper_rx_deferred;	/* frames enqueued to pending */
	atomic_t helper_rx_backpressure;/* dropped: pending queue full */
	atomic_t helper_rx_rssi_better;	/* helper RSSI > primary avg */
	atomic_t helper_rx_rssi_worse;	/* helper RSSI <= primary avg */
	atomic_t fallback_events;	/* helper disappeared/failed */
	atomic_t pair_events;		/* successful pairings */
	atomic_t unpair_events;		/* teardown events */
	atomic_t helper_rx_kern_crypto;	/* frames decrypted via kernel crypto */
	atomic_long_t helper_rx_bytes;	/* bytes delivered from helper frames */
};

/*
 * Non-QoS sequence number cache for duplicate detection.
 * The AMPDU reorder window handles dedup for QoS/AMPDU traffic,
 * but non-QoS frames need explicit tracking.
 */
struct coop_nonqos_seq_entry {
	u16 seq;
	u8 ta[ETH_ALEN];	/* per-STA key: same seq from different TAs is not a dup */
};

struct coop_nonqos_seq_cache {
	struct coop_nonqos_seq_entry entries[COOP_NONQOS_SEQ_CACHE_SZ];
	u32 valid;	/* bitmask of occupied slots (avoids seq 0 false positive) */
	u8 idx;
};

/*
 * Cooperative RX group — the central coordination object.
 * Spans across dvobj boundaries (separate USB devices).
 * Protected by spinlock for membership changes, RCU for hot-path reads.
 */
struct cooperative_rx_group {
	spinlock_t lock;
	enum coop_rx_state state;

	/* Primary adapter — the one with active STA or AP connection */
	_adapter *primary;
	struct dvobj_priv *primary_dvobj;

	/* Helper adapter(s) — contribute RX frames only */
	_adapter *helpers[COOP_MAX_HELPERS];
	struct dvobj_priv *helper_dvobjs[COOP_MAX_HELPERS];
	int num_helpers;

	/* Session binding — which BSS we're filtering for */
	u8 bound_bssid[ETH_ALEN];	/* STA: AP MAC, AP: own MAC */
	u8 bound_channel;
	u8 bound_bw;

	/* Non-QoS dedup cache — protected by its own lock to avoid
	 * contention with grp->lock (membership changes) */
	spinlock_t nonqos_lock;
	struct coop_nonqos_seq_cache nonqos_cache;

#ifdef CONFIG_COOP_RX_KERNEL_CRYPTO
	/* Kernel crypto transforms for accelerated helper decrypt.
	 * Allocated in process context (bind_session), used from
	 * softirq (drain_tasklet). NULL = fall back to SW path. */
	struct crypto_aead *tfm_ccm;	/* ccm(aes) for CCMP-128 */
	struct crypto_aead *tfm_ccm_256;/* ccm(aes) for CCMP-256 */
	struct crypto_aead *tfm_gcm;	/* gcm(aes) for GCMP-128/256 */
	/* Key caches — avoid redundant crypto_aead_setkey() calls.
	 * Group key: single-slot (all STAs share GTK).
	 * Pairwise key: tracked by STA MAC to handle AP mode where
	 * different STAs have different PTKs. Without per-STA tracking,
	 * interleaved frames from N STAs would thrash the cache. */
	u8 cached_gtk[32];		/* last group key set */
	u8 cached_gtk_len;
	struct crypto_aead *cached_gtk_tfm;

	u8 cached_ptk[32];		/* last pairwise key set */
	u8 cached_ptk_len;
	struct crypto_aead *cached_ptk_tfm;
	u8 cached_ptk_ta[ETH_ALEN];	/* STA whose PTK is cached */
	/* Pre-allocated AEAD request — avoids GFP_ATOMIC alloc/free
	 * per frame in the drain tasklet. Sized for the largest
	 * transform's reqsize. Only accessed from drain tasklet
	 * (serialized), so no locking needed. */
	struct aead_request *prealloc_req;
#endif

#ifdef CONFIG_COOP_RX_CAM_MIRROR
	/* CAM mirroring: program helper's HW crypto engine with
	 * primary's keys for zero-cost HW decrypt on helper frames.
	 * Set during bind_session, cleared during unbind/deinit. */
	u8 cam_mirror_active;
	u16 helper_seccfg_orig[COOP_MAX_HELPERS]; /* saved SECCFG */
#endif

	/* Deferred processing: helper enqueues, drain tasklet processes */
	_queue pending_queue;		/* validated frames awaiting processing */
	_tasklet coop_rx_tasklet;	/* drains pending_queue */
	atomic_t pending_count;		/* backpressure gauge */
	/*
	 * Tuning constants. PENDING_MAX scales with helper count to
	 * absorb AMPDU bursts without backpressure drops. Must leave
	 * headroom in the primary's 256-frame pool (NR_RECVFRAME)
	 * for its USB recv pipeline. With 4 helpers:
	 * 48 * (1+4) = 240, leaving 16 for primary — tight but OK
	 * since helpers share the pool transiently.
	 */
#define COOP_PENDING_BASE	48	/* per-helper burst budget */
#define coop_pending_max(grp)	(COOP_PENDING_BASE * (1 + READ_ONCE((grp)->num_helpers)))
#define COOP_BATCH_SIZE		64	/* frames per tasklet run */
#define COOP_RCU_BATCH		16	/* re-check RCU every N frames */

	/* Statistics */
	struct coop_rx_stats stats;

	/* Debugfs directory */
	struct dentry *debugfs_dir;
};

/* Global singleton — one cooperative group for the system */
extern struct cooperative_rx_group *rtw_coop_rx_group;
extern int rtw_cooperative_rx;	/* module parameter */
extern int rtw_coop_rx_drop_primary;	/* debug: drop primary RX */

/* Lifecycle */
int rtw_coop_rx_init(void);
void rtw_coop_rx_deinit(void);

/* Pairing */
int rtw_coop_rx_set_primary(_adapter *adapter);
int rtw_coop_rx_add_helper(_adapter *adapter);
int rtw_coop_rx_remove_helper(_adapter *adapter);
void rtw_coop_rx_remove_adapter(_adapter *adapter);
int rtw_coop_rx_bind_session(_adapter *primary);
void rtw_coop_rx_unbind_session(void);
int rtw_coop_rx_enable_helper_monitor(_adapter *helper, u8 channel);
void rtw_coop_rx_notify_channel_switch(_adapter *adapter);
void rtw_coop_rx_notify_gtk_rekey(_adapter *adapter);

/* Drain tasklet for deferred helper frame processing */
#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
void rtw_coop_rx_drain_tasklet(struct tasklet_struct *t);
#endif

/* Helper RX hot path — single entry point for pre_recv_entry() hook */
int rtw_coop_rx_pre_recv_entry(union recv_frame *precvframe,
			       _adapter *adapter, u8 *pbuf, u8 *pphy_status);
int rtw_coop_rx_submit_helper_frame(union recv_frame *precvframe,
				    _adapter *helper_adapter);

/* Query helpers — these are called from hot paths so must be fast */
static inline bool rtw_coop_rx_enabled(void)
{
	return READ_ONCE(rtw_cooperative_rx) != 0;
}

static inline bool rtw_coop_rx_active(void)
{
	struct cooperative_rx_group *grp;

	if (!rtw_coop_rx_enabled())
		return false;
	grp = READ_ONCE(rtw_coop_rx_group);
	return grp && READ_ONCE(grp->state) == COOP_STATE_ACTIVE;
}

static inline bool rtw_coop_rx_is_helper(_adapter *adapter)
{
	return READ_ONCE(adapter->is_coop_helper);
}

/* sysfs / proc interface */
int rtw_coop_rx_sysfs_init(struct net_device *ndev);
void rtw_coop_rx_sysfs_deinit(struct net_device *ndev);

/* debugfs */
int rtw_coop_rx_debugfs_init(void);
void rtw_coop_rx_debugfs_deinit(void);

/* Exported from os_intfs.c for driver-owned net_device validation */
#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29))
extern const struct net_device_ops rtw_netdev_ops;
#endif

#endif /* __RTW_COOPERATIVE_RX_H__ */
