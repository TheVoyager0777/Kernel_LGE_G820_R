/*
 * memcg_reclaim.c
 *
 * Copyright (C) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/dax.h>
#include <linux/psi.h>
#include <linux/sched/loadavg.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>
#include <linux/atomic.h>
#include <linux/swapops.h>
#include <linux/balloon_compaction.h>
#include <trace/events/vmscan.h>
#include <uapi/linux/sched/types.h>

#ifdef CONFIG_MEMCG_PROTECT_LRU
#include <linux/protect_lru.h>
#endif
#ifdef CONFIG_HP_CORE
#include <linux/hyperhold_inf.h>
#endif
#ifdef CONFIG_HW_RECLAIM_ACCT
#include <chipset_common/reclaim_acct/reclaim_acct.h>
#endif

#include <linux/memcg_policy.h>
#include "memcg_policy_internal.h"
#include "internal.h"

#ifndef CONFIG_REFAULT_IO_VMSCAN
static wait_queue_head_t snapshotd_wait;
static atomic_t snapshotd_wait_flag;
static atomic_t snapshotd_init_flag = ATOMIC_LONG_INIT(0);
static struct task_struct *snapshotd_task;
#else
/* anon:file cost pre refault cost ratio. use static to avoid iaware change */
#define STATIC_REFAULT_SWAPPINESS 170
/*
 * memcg anon refault threshold ratio, with swappiness,
 * means anon cost: file cost=4:1
 */
#define STATIC_REFAULT_MEMCG_RATIO 80

static int get_hp_swappiness()
{
	return hyperhold_enable() ? STATIC_REFAULT_SWAPPINESS : vm_swappiness;
}
#endif

static void zswapd_shrink_active_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, struct scan_control *sc, enum lru_list lru)
{
	unsigned long nr_scanned;
	unsigned long nr_taken;
	LIST_HEAD(l_hold);
	LIST_HEAD(l_inactive);
	struct page *page = NULL;
	unsigned int nr_deactivate;
	isolate_mode_t isolate_mode = 0;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
#ifndef CONFIG_REFAULT_IO_VMSCAN
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	struct zone_reclaim_stat *node_reclaim_stat =
		&pgdat->lruvec.reclaim_stat;
#endif

	lru_add_drain();

	spin_lock_irq(&pgdat->lru_lock);
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);
	__mod_node_page_state(pgdat, NR_ISOLATED_ANON, nr_taken);
#ifndef CONFIG_REFAULT_IO_VMSCAN
	reclaim_stat->recent_scanned[0] += nr_taken;
	node_reclaim_stat->recent_scanned[0] += nr_taken;
#endif
	__count_vm_events(PGREFILL, nr_scanned);
	count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);
	spin_unlock_irq(&pgdat->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

		ClearPageActive(page); /* we are de-activating */
		SetPageWorkingset(page);
		list_add(&page->lru, &l_inactive);
	}
	spin_lock_irq(&pgdat->lru_lock);
	nr_deactivate = move_active_pages_to_lru(lruvec, &l_inactive,
			&l_hold, lru - LRU_ACTIVE);
	/*lint -e501*/
	__mod_node_page_state(pgdat, NR_ISOLATED_ANON, -nr_taken);
	/*lint +e501*/
	spin_unlock_irq(&pgdat->lru_lock);
	mem_cgroup_uncharge_list(&l_hold);
	free_hot_cold_page_list(&l_hold, true);
	trace_mm_vmscan_lru_zswapd_shrink_active(pgdat->node_id,
			nr_taken, nr_deactivate, sc->priority);
}

static unsigned long move_pages_to_page_list(struct lruvec *lruvec,
		enum lru_list lru, struct list_head *page_list)
{
	unsigned long nr_isolated = 0;
	struct page *page = NULL;
	struct list_head *src = &lruvec->lists[lru];

	while (!list_empty(src)) {
		page = lru_to_page(src);
		if (PageUnevictable(page))
			continue;

		if (likely(get_page_unless_zero(page))) {
			if (isolate_lru_page(page)) {
				put_page(page);
				continue;
			}
			put_page(page);
		} else {
			continue;
		}

		if (PageUnevictable(page)) {
			putback_lru_page(page);
			continue;
		}

		/*
		 * MADV_FREE clears pte dirty bit and then marks the page
		 * lazyfree (clear SwapBacked). Inbetween if this lazyfreed page
		 * is touched by user then it becomes dirty.  PPR in
		 * shrink_page_list in try_to_unmap finds the page dirty, marks
		 * it back as PageSwapBacked and skips reclaim. This can cause
		 * isolated count mismatch.
		 */
		if (PageAnon(page) && !PageSwapBacked(page)) {
			putback_lru_page(page);
			continue;
		}

		list_add(&page->lru, page_list);
		nr_isolated++;
	}
	return nr_isolated;
}

unsigned long reclaim_all_anon_memcg(struct pglist_data *pgdat,
		struct mem_cgroup *memcg)
{
	LIST_HEAD(page_list);
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	unsigned long nr_reclaimed;
	struct page *page = NULL;
	struct reclaim_stat stat = {};
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
	};
#ifdef CONFIG_HW_RECLAIM_ACCT
	enum lru_list lru = LRU_INACTIVE_ANON;

	reclaimacct_shrinklist_start(is_file_lru(lru));
#endif
	count_vm_event(FREEZE_RECLAIM_TIMES);
	move_pages_to_page_list(lruvec,
			LRU_INACTIVE_ANON, &page_list);
	nr_reclaimed = shrink_page_list(&page_list, NULL, &sc,
			TTU_IGNORE_ACCESS, &stat, true);
	count_vm_events(FREEZE_RECLAIMED, nr_reclaimed);

	while (!list_empty(&page_list)) {
		page = lru_to_page(&page_list);
		list_del(&page->lru);
		putback_lru_page(page);
	}

#ifdef CONFIG_HW_RECLAIM_ACCT
	reclaimacct_shrinklist_end(is_file_lru(lru));
#endif
	return nr_reclaimed;
}

unsigned long reclaim_all_anon_memcg_prelaunch(struct pglist_data *pgdat,
		struct mem_cgroup *memcg)
{
	LIST_HEAD(page_list);
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	unsigned long nr_reclaimed;
	unsigned long nr_to_scan;
	unsigned long lru_active_anon_size;
	struct page *page = NULL;
	struct reclaim_stat stat = {};
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
	};

#ifdef CONFIG_HW_RECLAIM_ACCT
	enum lru_list lru = LRU_INACTIVE_ANON;

	reclaimacct_shrinklist_start(is_file_lru(lru));
#endif

	lru_active_anon_size = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
	while (lru_active_anon_size) {
		nr_to_scan = min(lru_active_anon_size, SWAP_CLUSTER_MAX);
		lru_active_anon_size -= nr_to_scan;
		zswapd_shrink_active_list(nr_to_scan, lruvec, &sc, LRU_ACTIVE_ANON);
	}
	move_pages_to_page_list(lruvec,
			LRU_INACTIVE_ANON, &page_list);
	nr_reclaimed = shrink_page_list(&page_list, NULL, &sc,
			TTU_IGNORE_ACCESS, &stat, true);

	while (!list_empty(&page_list)) {
		page = lru_to_page(&page_list);
		list_del(&page->lru);
		putback_lru_page(page);
	}

#ifdef CONFIG_HW_RECLAIM_ACCT
	reclaimacct_shrinklist_end(is_file_lru(lru));
#endif
	return nr_reclaimed;
}

#ifdef CONFIG_HYPERHOLD_FILE_LRU
static inline bool is_swap_not_allowed(struct scan_control *sc, int swappiness)
{
	return !sc->may_swap || !swappiness || !get_nr_swap_pages();
}

#ifdef CONFIG_DIRECT_SWAPPINESS
static inline int get_direct_swappiness(void)
{
	return direct_vm_swappiness;
}
#endif

#ifndef CONFIG_REFAULT_IO_VMSCAN
static void get_scan_count_hyperhold(struct pglist_data *pgdat,
		struct scan_control *sc, unsigned long *nr,
		unsigned long *lru_pages)
{
	int swappiness = vm_swappiness;
	struct lruvec *lruvec = node_lruvec(pgdat);
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	u64 fraction[2];
	u64 denominator;
	unsigned long anon_prio, file_prio;
	enum scan_balance scan_balance;
	unsigned long anon, file;
	unsigned long ap, fp;
	enum lru_list lru;
	unsigned long pgdatfile;
	unsigned long pgdatfree;
	int z;
	unsigned long total_high_wmark = 0;

#ifdef CONFIG_DIRECT_SWAPPINESS
	if (!current_is_kswapd())
		swappiness = get_direct_swappiness();

	pgdatfile = node_page_state(pgdat, NR_ACTIVE_FILE) +
		node_page_state(pgdat, NR_INACTIVE_FILE);
	if (pgdatfile <= 20 * SZ_1M / PAGE_SIZE)
		swappiness = 0;
#endif

	/* If we have no swap space, do not bother scanning anon pages. */
	if (is_swap_not_allowed(sc, swappiness)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	pgdatfree = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
	pgdatfile = node_page_state(pgdat, NR_ACTIVE_FILE) +
		node_page_state(pgdat, NR_INACTIVE_FILE);

	for (z = 0; z < MAX_NR_ZONES; z++) {
		struct zone *zone = &pgdat->node_zones[z];

		if (!managed_zone(zone))
			continue;

#ifdef CONFIG_ZONE_MEDIA
		if (IS_MEIDA_ZONE_IDX(z))
			continue;
#endif

		total_high_wmark += high_wmark_pages(zone);
	}

	if (unlikely(pgdatfile + pgdatfree <= total_high_wmark)) {
		/*
		 * Force SCAN_ANON if there are enough inactive
		 * anonymous pages on the LRU in eligible zones.
		 * Otherwise, the small LRU gets thrashed.
		 */
		if (!inactive_list_is_low(lruvec, false, sc, false) &&
			(lruvec_lru_size(lruvec, LRU_INACTIVE_ANON,
				sc->reclaim_idx) >>
				(unsigned int)sc->priority)) {
			scan_balance = SCAN_ANON;
			goto out;
		}
	}

	/*
	 * If there is enough inactive page cache, i.e. if the size of the
	 * inactive list is greater than that of the active list *and* the
	 * inactive list actually has some pages to scan on this priority, we
	 * do not reclaim anything from the anonymous working set right now.
	 * Without the second condition we could end up never scanning an
	 * lruvec even if it has plenty of old anonymous pages unless the
	 * system is under heavy pressure.
	 */
	if (enough_inactive_file == 0 &&
		!inactive_list_is_low(lruvec, true, sc, false) &&
		(lruvec_lru_size(lruvec, LRU_INACTIVE_FILE,
			sc->reclaim_idx) >> (unsigned int)sc->priority)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	anon_prio = swappiness;
	file_prio = 200 - anon_prio;

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */

	anon  = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES) +
		lruvec_lru_size(lruvec, LRU_INACTIVE_ANON, MAX_NR_ZONES);
	file  = lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES) +
		lruvec_lru_size(lruvec, LRU_INACTIVE_FILE, MAX_NR_ZONES);

	spin_lock_irq(&pgdat->lru_lock);
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
	}

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = anon_prio * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = file_prio * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;
	spin_unlock_irq(&pgdat->lru_lock);

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp + 1;
out:
	*lru_pages = 0;
	for_each_evictable_lru(lru) {
		int file; /*lint !e578*/
		unsigned long size;
		unsigned long scan;

		file = is_file_lru(lru);
		size = lruvec_lru_size(lruvec, lru, sc->reclaim_idx);
		scan = size >> (unsigned int)sc->priority;
		/*
		 * If the cgroup's already been deleted, make sure to
		 * scrape out the remaining cache.
		 */
		switch (scan_balance) {
		case SCAN_EQUAL:
			/* Scan lists relative to size */
			break;
		case SCAN_FRACT:
			/*
			 * Scan types proportional to swappiness and
			 * their relative recent reclaim efficiency.
			 * Make sure we don't miss the last page
			 * because of a round-off error.
			 */
			scan = DIV64_U64_ROUND_UP(scan * fraction[file],
					denominator); /*lint !e644*/
			break;
		case SCAN_FILE:
		case SCAN_ANON:
			/* Scan one type exclusively */
			if ((scan_balance == SCAN_FILE) != file) {
				size = 0;
				scan = 0;
			}
			break;
		default:
			break;
		}

		*lru_pages += size;
		nr[lru] = scan;
	}
}
#else
static void get_scan_count_hyperhold(struct pglist_data *pgdat,
		struct scan_control *sc, unsigned long *nr,
		unsigned long *lru_pages)
{
	int swappiness = get_hp_swappiness();
	struct lruvec *lruvec = node_lruvec(pgdat);
	u64 fraction[2];
	u64 denominator;
	enum scan_balance scan_balance;
	unsigned long anon_cost, file_cost, total_cost;
	unsigned long ap, fp;
	enum lru_list lru;
	unsigned long pgdatfile;

#ifdef CONFIG_DIRECT_SWAPPINESS
	if (!current_is_kswapd())
		swappiness = get_direct_swappiness();

	pgdatfile = node_page_state(pgdat, NR_ACTIVE_FILE) +
		node_page_state(pgdat, NR_INACTIVE_FILE);
	if (pgdatfile <= 20 * SZ_1M / PAGE_SIZE)
		swappiness = 0;
#endif

	/* If we have no swap space, do not bother scanning anon pages. */
	if (is_swap_not_allowed(sc, swappiness)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	if (sc->file_is_tiny) {
		scan_balance = SCAN_ANON;
		goto out;
	}

	if (sc->cache_trim_mode) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;

	/*
	 * Calculate the pressure balance between anon and file pages.
	 *
	 * The amount of pressure we put on each LRU is inversely
	 * proportional to the cost of reclaiming each list, as
	 * determined by the share of pages that are refaulting, times
	 * the relative IO cost of bringing back a swapped out
	 * anonymous page vs reloading a filesystem page (swappiness).
	 *
	 * Although we limit that influence to ensure no list gets
	 * left behind completely: at least a third of the pressure is
	 * applied, before swappiness.
	 *
	 * With swappiness at 100, anon and file have equal IO cost.
	 */
	total_cost = sc->anon_cost + sc->file_cost;
	anon_cost = total_cost + sc->anon_cost;
	file_cost = total_cost + sc->file_cost;
	total_cost = anon_cost + file_cost;

	ap = swappiness * (total_cost + 1);
	ap /= anon_cost + 1;

	fp = (200 - swappiness) * (total_cost + 1);
	fp /= file_cost + 1;

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp;
out:
	*lru_pages = 0;
	for_each_evictable_lru(lru) {
		int file; /*lint !e578*/
		unsigned long size;
		unsigned long scan;

		file = is_file_lru(lru);
		size = lruvec_lru_size(lruvec, lru, sc->reclaim_idx);
		scan = size >> (unsigned int)sc->priority;
		/*
		 * If the cgroup's already been deleted, make sure to
		 * scrape out the remaining cache.
		 */
		switch (scan_balance) {
		case SCAN_EQUAL:
			/* Scan lists relative to size */
			break;
		case SCAN_FRACT:
			/*
			 * Scan types proportional to swappiness and
			 * their relative recent reclaim efficiency.
			 * Make sure we don't miss the last page
			 * because of a round-off error.
			 */
			scan = DIV64_U64_ROUND_UP(scan * fraction[file],
					denominator); /*lint !e644*/
			break;
		case SCAN_FILE:
		case SCAN_ANON:
			/* Scan one type exclusively */
			if ((scan_balance == SCAN_FILE) != file) {
				size = 0;
				scan = 0;
			}
			break;
		default:
			break;
		}

		*lru_pages += size;
		nr[lru] = scan;
	}
}
#endif

#define ISOLATE_LIMIT_CNT 5
void shrink_anon_memcg(struct pglist_data *pgdat,
		struct mem_cgroup *memcg, struct scan_control *sc,
		unsigned long *nr)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	struct blk_plug plug;

	blk_start_plug(&plug);

	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_ANON]) {
		for (lru = 0; lru <= LRU_ACTIVE_ANON; lru++) {
			if (nr[lru]) {
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;
				nr_reclaimed +=
					shrink_list(lru, nr_to_scan,
							lruvec, sc);
			}
		}
		if (sc->nr_reclaimed >= sc->nr_to_reclaim ||
				(sc->isolate_count > ISOLATE_LIMIT_CNT &&
				sc->invoker == DIRECT_RECLAIM))
			break;
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;
	sc->nr_reclaimed_anon += nr_reclaimed;
}

static void shrink_anon(struct pglist_data *pgdat,
		struct scan_control *sc, unsigned long *nr)
{
	unsigned long reclaimed;
	unsigned long scanned;
	struct mem_cgroup *memcg = NULL;
	unsigned long nr_memcg[NR_LRU_LISTS];
	unsigned long nr_node_active = lruvec_lru_size(
			node_lruvec(pgdat), LRU_ACTIVE_ANON, MAX_NR_ZONES);
	unsigned long nr_node_inactive = lruvec_lru_size(
			node_lruvec(pgdat), LRU_INACTIVE_ANON, MAX_NR_ZONES);

	while ((memcg = get_next_memcg(memcg))) {
		struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);

		reclaimed = sc->nr_reclaimed;
		scanned = sc->nr_scanned;

		nr_memcg[LRU_ACTIVE_ANON] = nr[LRU_ACTIVE_ANON] *
			lruvec_lru_size(lruvec, LRU_ACTIVE_ANON,
					MAX_NR_ZONES) / (nr_node_active + 1);
		nr_memcg[LRU_INACTIVE_ANON] = nr[LRU_INACTIVE_ANON] *
			lruvec_lru_size(lruvec, LRU_INACTIVE_ANON,
					MAX_NR_ZONES) / (nr_node_inactive + 1);
		nr_memcg[LRU_ACTIVE_FILE] = 0;
		nr_memcg[LRU_INACTIVE_FILE] = 0;

		shrink_anon_memcg(pgdat, memcg, sc, nr_memcg);

		vmpressure(sc->gfp_mask, memcg, false,
				sc->nr_scanned - scanned,
				sc->nr_reclaimed - reclaimed);

		if (sc->nr_reclaimed >= sc->nr_to_reclaim ||
			(sc->isolate_count > ISOLATE_LIMIT_CNT &&
			sc->invoker == DIRECT_RECLAIM)) {
			get_next_memcg_break(memcg);
			break;
		}
	}
}

static void shrink_file(struct pglist_data *pgdat,
		struct scan_control *sc, unsigned long *nr)
{
	struct lruvec *lruvec = node_lruvec(pgdat);
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	struct blk_plug plug;

	blk_start_plug(&plug);

	while (nr[LRU_ACTIVE_FILE] || nr[LRU_INACTIVE_FILE]) {
		for (lru = LRU_INACTIVE_FILE; lru <= LRU_ACTIVE_FILE; lru++) {
			if (nr[lru]) {
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;
				nr_reclaimed +=
					shrink_list(lru,
							nr_to_scan,
							lruvec, sc);
			}
		}
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;
	sc->nr_reclaimed_file += nr_reclaimed;
}

#ifndef CONFIG_REFAULT_IO_VMSCAN
bool shrink_node_hyperhold(pg_data_t *pgdat, struct scan_control *sc)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	bool reclaimable = false;

#ifdef CONFIG_MEMCG_PROTECT_LRU
	shrink_prot_memcg_by_overratio();
#endif
	do {
		/* Get scan count for file and anon */
		unsigned long node_lru_pages = 0;
		unsigned long nr[NR_LRU_LISTS] = {0};

		nr_reclaimed = sc->nr_reclaimed;
		nr_scanned = sc->nr_scanned;

		get_scan_count_hyperhold(pgdat, sc, nr, &node_lru_pages);

		/* Shrink the Total-File-LRU */
		shrink_file(pgdat, sc, nr);

		/* Shrink Anon by iterating score_list */
		shrink_anon(pgdat, sc, nr);

		if (reclaim_state) {
			sc->nr_reclaimed += reclaim_state->reclaimed_slab;
			reclaim_state->reclaimed_slab = 0;
		}

		/* Record the subtree's reclaim efficiency */
		vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true,
			   sc->nr_scanned - nr_scanned,
			   sc->nr_reclaimed - nr_reclaimed);

		if (sc->nr_reclaimed - nr_reclaimed)
			reclaimable = true;
	} while (should_continue_reclaim(pgdat, sc->nr_reclaimed - nr_reclaimed,
					 sc->nr_scanned - nr_scanned, sc));

	/*
	 * Kswapd gives up on balancing particular nodes after too
	 * many failures to reclaim anything from them and goes to
	 * sleep. On reclaim progress, reset the failure counter. A
	 * successful direct reclaim run will revive a dormant kswapd.
	 */
	if (reclaimable)
		pgdat->kswapd_failures = 0;

	return reclaimable;
}
#else
bool shrink_node_hyperhold(pg_data_t *pgdat, struct scan_control *sc)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	unsigned long refaults, file;
	bool reclaimable = false;
	struct lruvec *target_lruvec = node_lruvec(pgdat);
	unsigned long total_high_wmark = 0;
	unsigned long free, anon;
	int z;

#ifdef CONFIG_MEMCG_PROTECT_LRU
	shrink_prot_memcg_by_overratio();
#endif
	do {
		/* Get scan count for file and anon */
		unsigned long node_lru_pages = 0;
		unsigned long nr[NR_LRU_LISTS] = {0};

		nr_reclaimed = sc->nr_reclaimed;
		nr_scanned = sc->nr_scanned;

		/*
		* Determine the scan balance between anon and file LRUs.
		*/
		spin_lock_irq(&pgdat->lru_lock);
		sc->anon_cost =
			lruvec_page_state(target_lruvec, WORKINGSET_ANON_COST);
		sc->file_cost =
			lruvec_page_state(target_lruvec, WORKINGSET_FILE_COST);
		spin_unlock_irq(&pgdat->lru_lock);

		/*
		* Target desirable inactive:active list ratios for the anon
		* and file LRU lists.
		*/
		if (!sc->force_deactivate) {
			refaults = lruvec_page_state(target_lruvec,
					WORKINGSET_ACTIVATE_ANON);
			if (refaults != target_lruvec->refaults[0] ||
				inactive_list_is_low(target_lruvec, false, sc, false))
				sc->may_deactivate |= DEACTIVATE_ANON;
			else
				sc->may_deactivate &= ~DEACTIVATE_ANON;

			/*
			* When refaults are being observed, it means a new
			* workingset is being established. Deactivate to get
			* rid of any stale active pages quickly.
			*/
			refaults = lruvec_page_state(target_lruvec,
					WORKINGSET_ACTIVATE_FILE);
			if (refaults != target_lruvec->refaults[1] ||
			inactive_list_is_low(target_lruvec, true, sc, false))
				sc->may_deactivate |= DEACTIVATE_FILE;
			else
				sc->may_deactivate &= ~DEACTIVATE_FILE;
		} else {
			sc->may_deactivate = DEACTIVATE_ANON | DEACTIVATE_FILE;
		}

		/*
		* If we have plenty of inactive file pages that aren't
		* thrashing, try to reclaim those first before touching
		* anonymous pages.
		*/
		file = lruvec_page_state(target_lruvec, NR_INACTIVE_FILE);
		if (file >> sc->priority && !(sc->may_deactivate & DEACTIVATE_FILE))
			sc->cache_trim_mode = 1;
		else
			sc->cache_trim_mode = 0;

		free = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
		file = node_page_state(pgdat, NR_ACTIVE_FILE) +
			node_page_state(pgdat, NR_INACTIVE_FILE);

		for (z = 0; z < MAX_NR_ZONES; z++) {
			struct zone *zone = &pgdat->node_zones[z];
			if (!managed_zone(zone))
				continue;

			total_high_wmark += high_wmark_pages(zone);
		}

		/*
		* Consider anon: if that's low too, this isn't a
		* runaway file reclaim problem, but rather just
		* extreme pressure. Reclaim as per usual then.
		*/
		anon = node_page_state(pgdat, NR_INACTIVE_ANON);

		sc->file_is_tiny =
			file + free <= total_high_wmark &&
			!(sc->may_deactivate & DEACTIVATE_ANON) &&
			anon >> sc->priority;

		get_scan_count_hyperhold(pgdat, sc, nr, &node_lru_pages);

		/* Shrink the Total-File-LRU */
		shrink_file(pgdat, sc, nr);

		/* Shrink Anon by iterating score_list */
		shrink_anon(pgdat, sc, nr);

		shrink_slab(sc->gfp_mask, pgdat->node_id, NULL,
				sc->nr_scanned - nr_scanned,
				node_lru_pages);

		if (reclaim_state) {
			sc->nr_reclaimed += reclaim_state->reclaimed_slab;
			reclaim_state->reclaimed_slab = 0;
		}

		/* Record the subtree's reclaim efficiency */
		vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true,
			   sc->nr_scanned - nr_scanned,
			   sc->nr_reclaimed - nr_reclaimed);

		if (sc->nr_reclaimed - nr_reclaimed)
			reclaimable = true;

	} while (should_continue_reclaim(pgdat, sc->nr_reclaimed - nr_reclaimed,
					 sc->nr_scanned - nr_scanned, sc));

	/*
	 * Kswapd gives up on balancing particular nodes after too
	 * many failures to reclaim anything from them and goes to
	 * sleep. On reclaim progress, reset the failure counter. A
	 * successful direct reclaim run will revive a dormant kswapd.
	 */
	if (reclaimable)
		pgdat->kswapd_failures = 0;

	return reclaimable;
}
#endif
#endif
