// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/compaction.c
 *
 * Memory compaction for the reduction of external fragmentation. Note that
 * this heavily depends upon page migration to do all the real heavy
 * lifting
 *
 * Copyright IBM Corp. 2007-2010 Mel Gorman <mel@csn.ul.ie>
 */
#include <linux/cpu.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/sched/signal.h>
#include <linux/backing-dev.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/page-isolation.h>
#include <linux/kasan.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/page_owner.h>
#include <linux/psi.h>
#include "internal.h"

#ifdef CONFIG_COMPACTION
static inline void count_compact_event(enum vm_event_item item)
{
	count_vm_event(item);
}

static inline void count_compact_events(enum vm_event_item item, long delta)
{
	count_vm_events(item, delta);
}
#else
#define count_compact_event(item) do { } while (0)
#define count_compact_events(item, delta) do { } while (0)
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

#define CREATE_TRACE_POINTS
#include <trace/events/compaction.h>

#define block_start_pfn(pfn, order)	round_down(pfn, 1UL << (order))
#define block_end_pfn(pfn, order)	ALIGN((pfn) + 1, 1UL << (order)) \
		/* pfn이 속하는 블록(2^9)의 시작 pfn */
#define pageblock_start_pfn(pfn)	block_start_pfn(pfn, pageblock_order) \
		/* pfn이 속하는 다음 블록(2^9)의 시작 pfn */
#define pageblock_end_pfn(pfn)		block_end_pfn(pfn, pageblock_order)

static unsigned long release_freepages(struct list_head *freelist)
{
	struct page *page, *next;
	unsigned long high_pfn = 0;

	list_for_each_entry_safe(page, next, freelist, lru) {
		unsigned long pfn = page_to_pfn(page);
		list_del(&page->lru);
		__free_page(page);
		if (pfn > high_pfn)
			high_pfn = pfn;
	}

	return high_pfn;
}

static void split_map_pages(struct list_head *list)
{
	unsigned int i, order, nr_pages;
	struct page *page, *next;
	LIST_HEAD(tmp_list);

	list_for_each_entry_safe(page, next, list, lru) {
		list_del(&page->lru);

		order = page_private(page);
		nr_pages = 1 << order;

		post_alloc_hook(page, order, __GFP_MOVABLE);
		if (order)
			split_page(page, order);

		for (i = 0; i < nr_pages; i++) {
			list_add(&page->lru, &tmp_list);
			page++;
		}
	}

	list_splice(&tmp_list, list);
}

#ifdef CONFIG_COMPACTION

int PageMovable(struct page *page)
{
	struct address_space *mapping;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	if (!__PageMovable(page))
		return 0;

	mapping = page_mapping(page);
	if (mapping && mapping->a_ops && mapping->a_ops->isolate_page)
		return 1;

	return 0;
}
EXPORT_SYMBOL(PageMovable);

void __SetPageMovable(struct page *page, struct address_space *mapping)
{
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE((unsigned long)mapping & PAGE_MAPPING_MOVABLE, page);
	page->mapping = (void *)((unsigned long)mapping | PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__SetPageMovable);

void __ClearPageMovable(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageMovable(page), page);
	/*
	 * Clear registered address_space val with keeping PAGE_MAPPING_MOVABLE
	 * flag so that VM can catch up released page by driver after isolation.
	 * With it, VM migration doesn't try to put it back.
	 */
	page->mapping = (void *)((unsigned long)page->mapping &
				PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__ClearPageMovable);

/* 64회 이상 compaction을 건너뛰지 마라 */
#define COMPACT_MAX_DEFER_SHIFT 6

/*
 * 페이지 할당 성공으로 발생하는 compaction 실패시 compaction이 유예된다.
 * 2의 compact_defer_limit승 compaction이 2의 COMPACT_MAX_DEFER_SHIFT승
 * 제한까지 건너뛴다.
 */
void defer_compaction(struct zone *zone, int order)
{
	zone->compact_considered = 0;
	zone->compact_defer_shift++;

	if (order < zone->compact_order_failed)
		zone->compact_order_failed = order;

	if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
		zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;

	trace_mm_compaction_defer_compaction(zone, order);
}

/* compaction이 이번에 건너뛰어야 하면 true 반환 */
bool compaction_deferred(struct zone *zone, int order)
{
	unsigned long defer_limit = 1UL << zone->compact_defer_shift;

	/*
	 * 요청 order가 이전 실패한 order 보다 작으면 compaction을 시도해
	 * 보기 위해 false 반환
	 */
	if (order < zone->compact_order_failed)
		return false;

	/* Avoid possible overflow */
	if (++zone->compact_considered > defer_limit)
		zone->compact_considered = defer_limit;

	/* defer_limit 에 도달한 경우도 false 반환 */
	if (zone->compact_considered >= defer_limit)
		return false;

	trace_mm_compaction_deferred(zone, order);

	return true;
}

/*
 * Update defer tracking counters after successful compaction of given order,
 * which means an allocation either succeeded (alloc_success == true) or is
 * expected to succeed.
 */
void compaction_defer_reset(struct zone *zone, int order,
		bool alloc_success)
{
	if (alloc_success) {
		zone->compact_considered = 0;
		zone->compact_defer_shift = 0;
	}
	if (order >= zone->compact_order_failed)
		zone->compact_order_failed = order + 1;

	trace_mm_compaction_defer_reset(zone, order);
}

/* Returns true if restarting compaction after many failures */
bool compaction_restarting(struct zone *zone, int order)
{
	if (order < zone->compact_order_failed)
		return false;

	return zone->compact_defer_shift == COMPACT_MAX_DEFER_SHIFT &&
		zone->compact_considered >= 1UL << zone->compact_defer_shift;
}

/* isolate를 위해 페이지가 스캔되어야 한다면 true를 반환한다 */
/* ignore_skip_hint 가 true 이거나 usemap 에 skip bit가 0 이면 true 반환 */
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	if (cc->ignore_skip_hint)
		return true;

	return !get_pageblock_skip(page);
}

static void reset_cached_positions(struct zone *zone)
{
	zone->compact_cached_migrate_pfn[0] = zone->zone_start_pfn;
	zone->compact_cached_migrate_pfn[1] = zone->zone_start_pfn;
	zone->compact_cached_free_pfn =
				pageblock_start_pfn(zone_end_pfn(zone) - 1);
}

/*
 * pageblock_order(9) 보다 크거나 같은 Compound 페이지는 해제될 때까지 계속하여
 * 건너뛰어야 한다. 그런 order(비록 migrate 가능하더라도)의 페이지 compaction은
 * 항상 무의미하고 그것들이 차지한 페이지는 free 페이지가 포함될 수 없다.
 */
/* order 9 이상의 compound 페이지일 경우 true 반환 */
static bool pageblock_skip_persistent(struct page *page)
{
	if (!PageCompound(page))
		return false;

	page = compound_head(page);

	if (compound_order(page) >= pageblock_order)
		return true;

	return false;
}

static bool
__reset_isolation_pfn(struct zone *zone, unsigned long pfn, bool check_source,
							bool check_target)
{
	struct page *page = pfn_to_online_page(pfn);
	struct page *block_page;
	struct page *end_page;
	unsigned long block_pfn;

	if (!page)
		return false;
	if (zone != page_zone(page))
		return false;
	if (pageblock_skip_persistent(page))
		return false;

	/*
	 * If skip is already cleared do no further checking once the
	 * restart points have been set.
	 */
	if (check_source && check_target && !get_pageblock_skip(page))
		return true;

	/*
	 * If clearing skip for the target scanner, do not select a
	 * non-movable pageblock as the starting point.
	 */
	if (!check_source && check_target &&
	    get_pageblock_migratetype(page) != MIGRATE_MOVABLE)
		return false;

	/* Ensure the start of the pageblock or zone is online and valid */
	block_pfn = pageblock_start_pfn(pfn);
	block_page = pfn_to_online_page(max(block_pfn, zone->zone_start_pfn));
	if (block_page) {
		page = block_page;
		pfn = block_pfn;
	}

	/* Ensure the end of the pageblock or zone is online and valid */
	block_pfn += pageblock_nr_pages;
	block_pfn = min(block_pfn, zone_end_pfn(zone) - 1);
	end_page = pfn_to_online_page(block_pfn);
	if (!end_page)
		return false;

	/*
	 * Only clear the hint if a sample indicates there is either a
	 * free page or an LRU page in the block. One or other condition
	 * is necessary for the block to be a migration source/target.
	 */
	do {
		if (pfn_valid_within(pfn)) {
			if (check_source && PageLRU(page)) {
				clear_pageblock_skip(page);
				return true;
			}

			if (check_target && PageBuddy(page)) {
				clear_pageblock_skip(page);
				return true;
			}
		}

		page += (1 << PAGE_ALLOC_COSTLY_ORDER);
		pfn += (1 << PAGE_ALLOC_COSTLY_ORDER);
	} while (page < end_page);

	return false;
}

/*
 * 이 함수는 migrate 와 free 스캐너가 만났을 때 페이지 isolation을 위해
 * 건너 뛰어야 하는 페이지블록의 모든 캐쉬된 정보를 지우기 위해 호출된다.
 */
static void __reset_isolation_suitable(struct zone *zone)
{
	unsigned long migrate_pfn = zone->zone_start_pfn;
	unsigned long free_pfn = zone_end_pfn(zone) - 1;
	unsigned long reset_migrate = free_pfn;
	unsigned long reset_free = migrate_pfn;
	bool source_set = false;
	bool free_set = false;

	if (!zone->compact_blockskip_flush)
		return;

	zone->compact_blockskip_flush = false;

	/*
	 * Walk the zone and update pageblock skip information. Source looks
	 * for PageLRU while target looks for PageBuddy. When the scanner
	 * is found, both PageBuddy and PageLRU are checked as the pageblock
	 * is suitable as both source and target.
	 */
	for (; migrate_pfn < free_pfn; migrate_pfn += pageblock_nr_pages,
					free_pfn -= pageblock_nr_pages) {
		cond_resched();

		/* Update the migrate PFN */
		if (__reset_isolation_pfn(zone, migrate_pfn, true, source_set) &&
		    migrate_pfn < reset_migrate) {
			source_set = true;
			reset_migrate = migrate_pfn;
			zone->compact_init_migrate_pfn = reset_migrate;
			zone->compact_cached_migrate_pfn[0] = reset_migrate;
			zone->compact_cached_migrate_pfn[1] = reset_migrate;
		}

		/* Update the free PFN */
		if (__reset_isolation_pfn(zone, free_pfn, free_set, true) &&
		    free_pfn > reset_free) {
			free_set = true;
			reset_free = free_pfn;
			zone->compact_init_free_pfn = reset_free;
			zone->compact_cached_free_pfn = reset_free;
		}
	}

	/* Leave no distance if no suitable block was reset */
	if (reset_migrate >= reset_free) {
		zone->compact_cached_migrate_pfn[0] = migrate_pfn;
		zone->compact_cached_migrate_pfn[1] = migrate_pfn;
		zone->compact_cached_free_pfn = free_pfn;
	}
}

void reset_isolation_suitable(pg_data_t *pgdat)
{
	int zoneid;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		struct zone *zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		/* Only flush if a full compaction finished recently */
		if (zone->compact_blockskip_flush)
			__reset_isolation_suitable(zone);
	}
}

/*
 * Sets the pageblock skip bit if it was clear. Note that this is a hint as
 * locks are not required for read/writers. Returns true if it was already set.
 */
static bool test_and_set_skip(struct compact_control *cc, struct page *page,
							unsigned long pfn)
{
	bool skip;

	/* Do no update if skip hint is being ignored */
	if (cc->ignore_skip_hint)
		return false;

	if (!IS_ALIGNED(pfn, pageblock_nr_pages))
		return false;

	skip = get_pageblock_skip(page);
	if (!skip && !cc->no_set_skip_hint)
		set_pageblock_skip(page);

	return skip;
}

/* pfn 매개변수가 속하는 블록의 다음 블록의 시작 PFN을 compact_cached_migrate_pfn */
/* 값으로 설정 */
static void update_cached_migrate(struct compact_control *cc, unsigned long pfn)
{
	struct zone *zone = cc->zone;

	pfn = pageblock_end_pfn(pfn);

	/* Set for isolation rather than compaction */
	if (cc->no_set_skip_hint)
		return;

	if (pfn > zone->compact_cached_migrate_pfn[0])
		zone->compact_cached_migrate_pfn[0] = pfn;
	if (cc->mode != MIGRATE_ASYNC &&
	    pfn > zone->compact_cached_migrate_pfn[1])
		zone->compact_cached_migrate_pfn[1] = pfn;
}

/*
 * If no pages were isolated then mark this pageblock to be skipped in the
 * future. The information is later cleared by __reset_isolation_suitable().
 */
static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long pfn)
{
	struct zone *zone = cc->zone;

	if (cc->no_set_skip_hint)
		return;

	if (!page)
		return;

	set_pageblock_skip(page);

	/* Update where async and sync compaction should restart */
	if (pfn < zone->compact_cached_free_pfn)
		zone->compact_cached_free_pfn = pfn;
}
#else
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	return true;
}

static inline bool pageblock_skip_persistent(struct page *page)
{
	return false;
}

static inline void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long pfn)
{
}

static void update_cached_migrate(struct compact_control *cc, unsigned long pfn)
{
}

static bool test_and_set_skip(struct compact_control *cc, struct page *page,
							unsigned long pfn)
{
	return false;
}
#endif /* CONFIG_COMPACTION */

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. For async compaction, trylock and record if the
 * lock is contended. The lock will still be acquired but compaction will
 * abort when the current block is finished regardless of success rate.
 * Sync compaction acquires the lock.
 *
 * Always returns true which makes it easier to track lock state in callers.
 */
static bool compact_lock_irqsave(spinlock_t *lock, unsigned long *flags,
						struct compact_control *cc)
{
	/* Track if the lock is contended in async mode */
	if (cc->mode == MIGRATE_ASYNC && !cc->contended) {
		if (spin_trylock_irqsave(lock, *flags))
			return true;

		cc->contended = true;
	}

	spin_lock_irqsave(lock, *flags);
	return true;
}

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. The lock should be periodically unlocked to avoid
 * having disabled IRQs for a long time, even when there is nobody waiting on
 * the lock. It might also be that allowing the IRQs will result in
 * need_resched() becoming true. If scheduling is needed, async compaction
 * aborts. Sync compaction schedules.
 * Either compaction type will also abort if a fatal signal is pending.
 * In either case if the lock was locked, it is dropped and not regained.
 *
 * Returns true if compaction should abort due to fatal signal pending, or
 *		async compaction due to need_resched()
 * Returns false when compaction can continue (sync compaction might have
 *		scheduled)
 */
/*
 * compaction 은 잠재적으로 매우 심하게 경합될 수 있는 거친 락을 가져와야 한다. 아무도
 * lock을 기다리지 않을 때 조차 긴시간 IRQ가 비활성화되는 것을 피하기 위해 주기적으로
 * 락을 해제해야 한다.
 * IRQ를 허용하면 need_resched()함수가 true 가 될 수 있다. 만약 스케쥴링이 필요하면
 * 비동기 compaction은 중지된다. 동기 compaction은 스케쥴된다. fatal signal이
 * 보류중이면 두 compaction 유형은 중지된다. 두경우 잠금되었다면 삭제되고 다시
 * 회복되지 않는다.
 * fatal signal 보류로 인해서 compaction이 중지되어야 하거나 비동기 compaction이
 * need_resched() 로 인해 중지되어야 하면 true 를 반환.
 * compaction 이 계속되어야 하면 true 반환(동기 compaction은 스케쥴되었을 수 있음.)
 */
static bool compact_unlock_should_abort(spinlock_t *lock,
		unsigned long flags, bool *locked, struct compact_control *cc)
{
	if (*locked) {
		spin_unlock_irqrestore(lock, flags);
		*locked = false;
	}

	if (fatal_signal_pending(current)) {
		cc->contended = true;
		return true;
	}

	cond_resched();

	return false;
}

/*
 * Isolate free pages onto a private freelist. If @strict is true, will abort
 * returning 0 on any invalid PFNs or non-free pages inside of the pageblock
 * (even though it may still end up isolating some pages).
 */
static unsigned long isolate_freepages_block(struct compact_control *cc,
				unsigned long *start_pfn,
				unsigned long end_pfn,
				struct list_head *freelist,
				unsigned int stride,
				bool strict)
{
	int nr_scanned = 0, total_isolated = 0;
	struct page *cursor;
	unsigned long flags = 0;
	bool locked = false;
	unsigned long blockpfn = *start_pfn;
	unsigned int order;

	/* Strict mode is for isolation, speed is secondary */
	if (strict)
		stride = 1;

	cursor = pfn_to_page(blockpfn);

	/* Isolate free pages. */
	for (; blockpfn < end_pfn; blockpfn += stride, cursor += stride) {
		int isolated;
		struct page *page = cursor;

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort if fatal signal
		 * pending or async compaction detects need_resched()
		 */
		if (!(blockpfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&cc->zone->lock, flags,
								&locked, cc))
			break;

		nr_scanned++;
		if (!pfn_valid_within(blockpfn))
			goto isolate_fail;

		/*
		 * For compound pages such as THP and hugetlbfs, we can save
		 * potentially a lot of iterations if we skip them at once.
		 * The check is racy, but we can consider only valid values
		 * and the only danger is skipping too much.
		 */
		if (PageCompound(page)) {
			const unsigned int order = compound_order(page);

			if (likely(order < MAX_ORDER)) {
				blockpfn += (1UL << order) - 1;
				cursor += (1UL << order) - 1;
			}
			goto isolate_fail;
		}

		if (!PageBuddy(page))
			goto isolate_fail;

		/*
		 * If we already hold the lock, we can skip some rechecking.
		 * Note that if we hold the lock now, checked_pageblock was
		 * already set in some previous iteration (or strict is true),
		 * so it is correct to skip the suitable migration target
		 * recheck as well.
		 */
		if (!locked) {
			locked = compact_lock_irqsave(&cc->zone->lock,
								&flags, cc);

			/* Recheck this is a buddy page under lock */
			if (!PageBuddy(page))
				goto isolate_fail;
		}

		/* Found a free page, will break it into order-0 pages */
		order = page_order(page);
		isolated = __isolate_free_page(page, order);
		if (!isolated)
			break;
		set_page_private(page, order);

		total_isolated += isolated;
		cc->nr_freepages += isolated;
		list_add_tail(&page->lru, freelist);

		if (!strict && cc->nr_migratepages <= cc->nr_freepages) {
			blockpfn += isolated;
			break;
		}
		/* Advance to the end of split page */
		blockpfn += isolated - 1;
		cursor += isolated - 1;
		continue;

isolate_fail:
		if (strict)
			break;
		else
			continue;

	}

	if (locked)
		spin_unlock_irqrestore(&cc->zone->lock, flags);

	/*
	 * There is a tiny chance that we have read bogus compound_order(),
	 * so be careful to not go outside of the pageblock.
	 */
	if (unlikely(blockpfn > end_pfn))
		blockpfn = end_pfn;

	trace_mm_compaction_isolate_freepages(*start_pfn, blockpfn,
					nr_scanned, total_isolated);

	/* Record how far we have got within the block */
	*start_pfn = blockpfn;

	/*
	 * If strict isolation is requested by CMA then check that all the
	 * pages requested were isolated. If there were any failures, 0 is
	 * returned and CMA will fail.
	 */
	if (strict && blockpfn < end_pfn)
		total_isolated = 0;

	cc->total_free_scanned += nr_scanned;
	if (total_isolated)
		count_compact_events(COMPACTISOLATED, total_isolated);
	return total_isolated;
}

/**
 * isolate_freepages_range() - isolate free pages.
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Non-free pages, invalid PFNs, or zone boundaries within the
 * [start_pfn, end_pfn) range are considered errors, cause function to
 * undo its actions and return zero.
 *
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater then end_pfn if end fell in a middle of
 * a free page).
 */
unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long isolated, pfn, block_start_pfn, block_end_pfn;
	LIST_HEAD(freelist);

	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn += isolated,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {
		/* Protect pfn from changing by isolate_freepages_block */
		unsigned long isolate_start_pfn = pfn;

		block_end_pfn = min(block_end_pfn, end_pfn);

		/*
		 * pfn could pass the block_end_pfn if isolated freepage
		 * is more than pageblock order. In this case, we adjust
		 * scanning range to right one.
		 */
		if (pfn >= block_end_pfn) {
			block_start_pfn = pageblock_start_pfn(pfn);
			block_end_pfn = pageblock_end_pfn(pfn);
			block_end_pfn = min(block_end_pfn, end_pfn);
		}

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			break;

		isolated = isolate_freepages_block(cc, &isolate_start_pfn,
					block_end_pfn, &freelist, 0, true);

		/*
		 * In strict mode, isolate_freepages_block() returns 0 if
		 * there are any holes in the block (ie. invalid PFNs or
		 * non-free pages).
		 */
		if (!isolated)
			break;

		/*
		 * If we managed to isolate pages, it is always (1 << n) *
		 * pageblock_nr_pages for some non-negative n.  (Max order
		 * page may span two pageblocks).
		 */
	}

	/* __isolate_free_page() does not map the pages */
	split_map_pages(&freelist);

	if (pfn < end_pfn) {
		/* Loop terminated early, cleanup. */
		release_freepages(&freelist);
		return 0;
	}

	/* We don't use freelists for anything. */
	return pfn;
}

/* Similar to reclaim, but different enough that they don't share logic */
static bool too_many_isolated(pg_data_t *pgdat)
{
	unsigned long active, inactive, isolated;

	inactive = node_page_state(pgdat, NR_INACTIVE_FILE) +
			node_page_state(pgdat, NR_INACTIVE_ANON);
	active = node_page_state(pgdat, NR_ACTIVE_FILE) +
			node_page_state(pgdat, NR_ACTIVE_ANON);
	isolated = node_page_state(pgdat, NR_ISOLATED_FILE) +
			node_page_state(pgdat, NR_ISOLATED_ANON);

	return isolated > (inactive + active) / 2;
}

/**
 * isolate_migratepages_block() - 단일 페이지블록 내에서 마이그레이션 가능한 모든
 *				  페이지를 분리
 * @cc:		Compaction control structure.
 * @low_pfn:	The first PFN to isolate
 * @end_pfn:	The one-past-the-last PFN to isolate, within same pageblock
 * @isolate_mode: Isolation mode to be used.
 *
 * [low_pfn, end_pfn)에 지정된 범위에서 마이그레이션 할 수있는 모든 페이지를 분리합니다.
 * 범위는 동일한 페이지 블록 내에 있어야합니다. 보류중인 fatal signal이 있으면
 * 0을 반환하고 그렇지 않으면 스캔되지 않은 첫 번째 페이지의 PFN을 반환합니다
 * (둘 다 end_pfn보다 작거나 같거나 클 수 있음).
 *
 * 페이지는 cc->migratepages 목록 (비어 있지 않아도 됨)에 분리되어 있으며
 * cc->nr_migratepages는 그에 따라 업데이트됩니다.
 * cc-> migrate_pfn 필드는 읽거나 업데이트되지 않습니다.
 */
static unsigned long
isolate_migratepages_block(struct compact_control *cc, unsigned long low_pfn,
			unsigned long end_pfn, isolate_mode_t isolate_mode)
{
	pg_data_t *pgdat = cc->zone->zone_pgdat;
	unsigned long nr_scanned = 0, nr_isolated = 0;
	struct lruvec *lruvec;
	unsigned long flags = 0;
	bool locked = false;
	struct page *page = NULL, *valid_page = NULL;
	unsigned long start_pfn = low_pfn;
	bool skip_on_failure = false;
	unsigned long next_skip_pfn = 0;
	bool skip_updated = false;

	/*
	 * 병렬 reclaimers나 compaction에 의해 LRU 목록에서 분리된 페이지가
	 * 너무 많지 않은지 확인하라. 만약 그렇다면 적은 수의 페이지들이 분리되도록
	 * 잠시 지연하라.
	 */
	while (unlikely(too_many_isolated(pgdat))) {
		/* async migration should just abort */
		if (cc->mode == MIGRATE_ASYNC)
			return 0;

		congestion_wait(BLK_RW_ASYNC, HZ/10);

		if (fatal_signal_pending(current))
			return 0;
	}

	cond_resched();

	if (cc->direct_compaction && (cc->mode == MIGRATE_ASYNC)) {
		skip_on_failure = true;
		next_skip_pfn = block_end_pfn(low_pfn, cc->order);
	}

	/* Time to isolate some pages for migration */
	for (; low_pfn < end_pfn; low_pfn++) {

		if (skip_on_failure && low_pfn >= next_skip_pfn) {
			/*
			 * 이전 order 정렬된 블록의 모든 migration 후보를
			 * 격리하였고 실패로 건너뛰지 않았다. 이제 페이지들을 migrate
			 * 해야하고 compaction에 성공해야 한다.
			 */
			if (nr_isolated)
				break;

			/*
			 * 이전 order 정렬 블록의 격리에 실패했다. 현재 블록의
			 * 마지막에 새 경계를 설정하라. 우리는 간단히 2의 order 승으로
			 * next_skip_pfn을 증가시킬 수 없는데 low_pfn이 이전
			 * 루프 반복에서 compound나 high-order 버디 페이지의
			 * 건너뛰기로 인해 높은 수로 증가되었을 수 있기 때문이다
			 */
			next_skip_pfn = block_end_pfn(low_pfn, cc->order);
		}

		/*
		 * 주기적으로 IRQ에 기회를 주기 위해 경합에 관계없이 lock(보유한 경우)을
		 * 삭제하라. 경합하는 경우 비동기 compaction 은 중단하라.
		 */
		if (!(low_pfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&pgdat->lru_lock,
					    flags, &locked, cc))
			break;

		if (!pfn_valid_within(low_pfn))
			goto isolate_fail;
		nr_scanned++;

		page = pfn_to_page(low_pfn);

		/*
		 * 페이지블록이 이미 건너뛰기로 표시되었는지 확인하라.
		 * 호출자가 한번에 COMPACT_CLUSTER_MAX 를 분리하므로
		 * 정렬된 PFN 만이 검사된다. 그래서 두번째 호출에서 블록이 건너 뛰어야
		 * 한다고 잘못 결정내려서는 안된다.
		 */
		if (!valid_page && IS_ALIGNED(low_pfn, pageblock_nr_pages)) {
			if (!cc->ignore_skip_hint && get_pageblock_skip(page)) {
				low_pfn = end_pfn;
				goto isolate_abort;
			}
			valid_page = page;
		}

		/*
		 * free하면 건너뛰라. 보통 안전하지 않지만 zone lock 없이 여기에서
		 * page order 를 읽는다. 그러나 race window 는 작고 일어날 수 있는
		 * 최악의 상황은 약간의 잠재적 isolation targets을 건너 뛰는 것이다
		 */
		if (PageBuddy(page)) {
			unsigned long freepage_order = page_order_unsafe(page);

			/*
			 * lock 없이 유효한 페이지 order 인지 확신할 수 없다.
			 * low_pfn overflow 를 막기위해 유효한 order영역 값인지만
			 * 고려하라.
			 */
			if (freepage_order > 0 && freepage_order < MAX_ORDER)
				low_pfn += (1UL << freepage_order) - 1;
			continue;
		}

		/*
		 * LRU 페이지와 관계없이, THP와 hugetlbfs 같은 compound 페이지는
		 * compaction 안된다. 한번에 그것들을 건너뛰면 잠재적으로 많은
		 * 반복을 절약할 수 있다. check는 racy 하지만 유효값만 고려할 수
		 * 있고 위험은 너무 많이 건너뛰는 것 뿐이다.
		 */
		if (PageCompound(page)) {
			const unsigned int order = compound_order(page);

			if (likely(order < MAX_ORDER))
				low_pfn += (1UL << order) - 1;
			goto isolate_fail;
		}

		/*
		 * 검사는 락이 없을 수 있지만 후에 재검사 하므로 괜찮다.
		 * migrate LRU 와 non-lru movable 페이지 외에는 건너뛰라.
		 */
		if (!PageLRU(page)) {
			/*
			 * __PageMovable은 오탐을 반환할 수 있으므로
			 * page_lock 에서 확인해야 한다.
			 */
			if (unlikely(__PageMovable(page)) &&
					!PageIsolated(page)) {
				if (locked) {
					spin_unlock_irqrestore(&pgdat->lru_lock,
									flags);
					locked = false;
				}

				if (!isolate_movable_page(page, isolate_mode))
					goto isolate_success;
			}

			goto isolate_fail;
		}

		/*
		 * Migration will fail if an anonymous page is pinned in memory,
		 * so avoid taking lru_lock and isolating it unnecessarily in an
		 * admittedly racy check.
		 */
		if (!page_mapping(page) &&
		    page_count(page) > page_mapcount(page))
			goto isolate_fail;

		/*
		 * Only allow to migrate anonymous pages in GFP_NOFS context
		 * because those do not depend on fs locks.
		 */
		if (!(cc->gfp_mask & __GFP_FS) && page_mapping(page))
			goto isolate_fail;

		/* If we already hold the lock, we can skip some rechecking */
		if (!locked) {
			locked = compact_lock_irqsave(&pgdat->lru_lock,
								&flags, cc);

			/* Try get exclusive access under lock */
			if (!skip_updated) {
				skip_updated = true;
				if (test_and_set_skip(cc, page, low_pfn))
					goto isolate_abort;
			}

			/* Recheck PageLRU and PageCompound under lock */
			if (!PageLRU(page))
				goto isolate_fail;

			/*
			 * Page become compound since the non-locked check,
			 * and it's on LRU. It can only be a THP so the order
			 * is safe to read and it's 0 for tail pages.
			 */
			if (unlikely(PageCompound(page))) {
				low_pfn += (1UL << compound_order(page)) - 1;
				goto isolate_fail;
			}
		}

		lruvec = mem_cgroup_page_lruvec(page, pgdat);

		/* Try isolate the page */
		if (__isolate_lru_page(page, isolate_mode) != 0)
			goto isolate_fail;

		VM_BUG_ON_PAGE(PageCompound(page), page);

		/* Successfully isolated */
		del_page_from_lru_list(page, lruvec, page_lru(page));
		inc_node_page_state(page,
				NR_ISOLATED_ANON + page_is_file_cache(page));

isolate_success:
		list_add(&page->lru, &cc->migratepages);
		cc->nr_migratepages++;
		nr_isolated++;

		/*
		 * Avoid isolating too much unless this block is being
		 * rescanned (e.g. dirty/writeback pages, parallel allocation)
		 * or a lock is contended. For contention, isolate quickly to
		 * potentially remove one source of contention.
		 */
		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX &&
		    !cc->rescan && !cc->contended) {
			++low_pfn;
			break;
		}

		continue;
isolate_fail:
		if (!skip_on_failure)
			continue;

		/*
		 * We have isolated some pages, but then failed. Release them
		 * instead of migrating, as we cannot form the cc->order buddy
		 * page anyway.
		 */
		if (nr_isolated) {
			if (locked) {
				spin_unlock_irqrestore(&pgdat->lru_lock, flags);
				locked = false;
			}
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			nr_isolated = 0;
		}

		if (low_pfn < next_skip_pfn) {
			low_pfn = next_skip_pfn - 1;
			/*
			 * The check near the loop beginning would have updated
			 * next_skip_pfn too, but this is a bit simpler.
			 */
			next_skip_pfn += 1UL << cc->order;
		}
	}

	/*
	 * The PageBuddy() check could have potentially brought us outside
	 * the range to be scanned.
	 */
	if (unlikely(low_pfn > end_pfn))
		low_pfn = end_pfn;

isolate_abort:
	if (locked)
		spin_unlock_irqrestore(&pgdat->lru_lock, flags);

	/*
	 * Updated the cached scanner pfn once the pageblock has been scanned
	 * Pages will either be migrated in which case there is no point
	 * scanning in the near future or migration failed in which case the
	 * failure reason may persist. The block is marked for skipping if
	 * there were no pages isolated in the block or if the block is
	 * rescanned twice in a row.
	 */
	if (low_pfn == end_pfn && (!nr_isolated || cc->rescan)) {
		if (valid_page && !skip_updated)
			set_pageblock_skip(valid_page);
		update_cached_migrate(cc, low_pfn);
	}

	trace_mm_compaction_isolate_migratepages(start_pfn, low_pfn,
						nr_scanned, nr_isolated);

	cc->total_migrate_scanned += nr_scanned;
	if (nr_isolated)
		count_compact_events(COMPACTISOLATED, nr_isolated);

	return low_pfn;
}

/**
 * isolate_migratepages_range() - isolate migrate-able pages in a PFN range
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Returns zero if isolation fails fatally due to e.g. pending signal.
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater than end_pfn if end fell in a middle of a THP page).
 */
unsigned long
isolate_migratepages_range(struct compact_control *cc, unsigned long start_pfn,
							unsigned long end_pfn)
{
	unsigned long pfn, block_start_pfn, block_end_pfn;

	/* Scan block by block. First and last block may be incomplete */
	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn = block_end_pfn,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {

		block_end_pfn = min(block_end_pfn, end_pfn);

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			continue;

		pfn = isolate_migratepages_block(cc, pfn, block_end_pfn,
							ISOLATE_UNEVICTABLE);

		if (!pfn)
			break;

		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX)
			break;
	}

	return pfn;
}

#endif /* CONFIG_COMPACTION || CONFIG_CMA */
#ifdef CONFIG_COMPACTION

/* 요청 migrate type과 현재 페이지의 migrate type이 같은 경우 true 반환 */
/* 동기 모드이거나 direct_compaction이 아닌 경우도 true 반환 */
/* order 9 이상의 compound page는 무조건 false */
static bool suitable_migration_source(struct compact_control *cc,
							struct page *page)
{
	int block_mt;

	if (pageblock_skip_persistent(page))
		return false;

	if ((cc->mode != MIGRATE_ASYNC) || !cc->direct_compaction)
		return true;

	block_mt = get_pageblock_migratetype(page);

	if (cc->migratetype == MIGRATE_MOVABLE)
		return is_migrate_movable(block_mt);
	else
		return block_mt == cc->migratetype;
}

/* Returns true if the page is within a block suitable for migration to */
static bool suitable_migration_target(struct compact_control *cc,
							struct page *page)
{
	/* If the page is a large free page, then disallow migration */
	if (PageBuddy(page)) {
		/*
		 * We are checking page_order without zone->lock taken. But
		 * the only small danger is that we skip a potentially suitable
		 * pageblock, so it's not worth to check order for valid range.
		 */
		if (page_order_unsafe(page) >= pageblock_order)
			return false;
	}

	if (cc->ignore_block_suitable)
		return true;

	/* If the block is MIGRATE_MOVABLE or MIGRATE_CMA, allow migration */
	if (is_migrate_movable(get_pageblock_migratetype(page)))
		return true;

	/* Otherwise skip the block */
	return false;
}

static inline unsigned int
freelist_scan_limit(struct compact_control *cc)
{
	return (COMPACT_CLUSTER_MAX >> cc->fast_search_fail) + 1;
}

/*
 * free 스캐너가 migration 스캐너 보다 같거나 작은 페이비블록을 가지는지 테스트해서
 * compaction 을 종료해야 한다.
 */
static inline bool compact_scanners_met(struct compact_control *cc)
{
	return (cc->free_pfn >> pageblock_order)
		<= (cc->migrate_pfn >> pageblock_order);
}

/*
 * Used when scanning for a suitable migration target which scans freelists
 * in reverse. Reorders the list such as the unscanned pages are scanned
 * first on the next iteration of the free scanner
 */
static void
move_freelist_head(struct list_head *freelist, struct page *freepage)
{
	LIST_HEAD(sublist);

	if (!list_is_last(freelist, &freepage->lru)) {
		list_cut_before(&sublist, freelist, &freepage->lru);
		if (!list_empty(&sublist))
			list_splice_tail(&sublist, freelist);
	}
}

/*
 * Similar to move_freelist_head except used by the migration scanner
 * when scanning forward. It's possible for these list operations to
 * move against each other if they search the free list exactly in
 * lockstep.
 */
static void
move_freelist_tail(struct list_head *freelist, struct page *freepage)
{
	LIST_HEAD(sublist);

	if (!list_is_first(freelist, &freepage->lru)) {
		list_cut_position(&sublist, freelist, &freepage->lru);
		if (!list_empty(&sublist))
			list_splice_tail(&sublist, freelist);
	}
}

static void
fast_isolate_around(struct compact_control *cc, unsigned long pfn, unsigned long nr_isolated)
{
	unsigned long start_pfn, end_pfn;
	struct page *page = pfn_to_page(pfn);

	/* Do not search around if there are enough pages already */
	if (cc->nr_freepages >= cc->nr_migratepages)
		return;

	/* Minimise scanning during async compaction */
	if (cc->direct_compaction && cc->mode == MIGRATE_ASYNC)
		return;

	/* Pageblock boundaries */
	start_pfn = pageblock_start_pfn(pfn);
	end_pfn = min(start_pfn + pageblock_nr_pages, zone_end_pfn(cc->zone));

	/* Scan before */
	if (start_pfn != pfn) {
		isolate_freepages_block(cc, &start_pfn, pfn, &cc->freepages, 1, false);
		if (cc->nr_freepages >= cc->nr_migratepages)
			return;
	}

	/* Scan after */
	start_pfn = pfn + nr_isolated;
	if (start_pfn != end_pfn)
		isolate_freepages_block(cc, &start_pfn, end_pfn, &cc->freepages, 1, false);

	/* Skip this pageblock in the future as it's full or nearly full */
	if (cc->nr_freepages < cc->nr_migratepages)
		set_pageblock_skip(page);
}

/* Search orders in round-robin fashion */
static int next_search_order(struct compact_control *cc, int order)
{
	order--;
	if (order < 0)
		order = cc->order - 1;

	/* Search wrapped around? */
	if (order == cc->search_order) {
		cc->search_order--;
		if (cc->search_order < 0)
			cc->search_order = cc->order - 1;
		return -1;
	}

	return order;
}

static unsigned long
fast_isolate_freepages(struct compact_control *cc)
{
	unsigned int limit = min(1U, freelist_scan_limit(cc) >> 1);
	unsigned int nr_scanned = 0;
	unsigned long low_pfn, min_pfn, high_pfn = 0, highest = 0;
	unsigned long nr_isolated = 0;
	unsigned long distance;
	struct page *page = NULL;
	bool scan_start = false;
	int order;

	/* Full compaction passes in a negative order */
	if (cc->order <= 0)
		return cc->free_pfn;

	/*
	 * If starting the scan, use a deeper search and use the highest
	 * PFN found if a suitable one is not found.
	 */
	if (cc->free_pfn >= cc->zone->compact_init_free_pfn) {
		limit = pageblock_nr_pages >> 1;
		scan_start = true;
	}

	/*
	 * Preferred point is in the top quarter of the scan space but take
	 * a pfn from the top half if the search is problematic.
	 */
	distance = (cc->free_pfn - cc->migrate_pfn);
	low_pfn = pageblock_start_pfn(cc->free_pfn - (distance >> 2));
	min_pfn = pageblock_start_pfn(cc->free_pfn - (distance >> 1));

	if (WARN_ON_ONCE(min_pfn > low_pfn))
		low_pfn = min_pfn;

	/*
	 * Search starts from the last successful isolation order or the next
	 * order to search after a previous failure
	 */
	cc->search_order = min_t(unsigned int, cc->order - 1, cc->search_order);

	for (order = cc->search_order;
	     !page && order >= 0;
	     order = next_search_order(cc, order)) {
		struct free_area *area = &cc->zone->free_area[order];
		struct list_head *freelist;
		struct page *freepage;
		unsigned long flags;
		unsigned int order_scanned = 0;

		if (!area->nr_free)
			continue;

		spin_lock_irqsave(&cc->zone->lock, flags);
		freelist = &area->free_list[MIGRATE_MOVABLE];
		list_for_each_entry_reverse(freepage, freelist, lru) {
			unsigned long pfn;

			order_scanned++;
			nr_scanned++;
			pfn = page_to_pfn(freepage);

			if (pfn >= highest)
				highest = pageblock_start_pfn(pfn);

			if (pfn >= low_pfn) {
				cc->fast_search_fail = 0;
				cc->search_order = order;
				page = freepage;
				break;
			}

			if (pfn >= min_pfn && pfn > high_pfn) {
				high_pfn = pfn;

				/* Shorten the scan if a candidate is found */
				limit >>= 1;
			}

			if (order_scanned >= limit)
				break;
		}

		/* Use a minimum pfn if a preferred one was not found */
		if (!page && high_pfn) {
			page = pfn_to_page(high_pfn);

			/* Update freepage for the list reorder below */
			freepage = page;
		}

		/* Reorder to so a future search skips recent pages */
		move_freelist_head(freelist, freepage);

		/* Isolate the page if available */
		if (page) {
			if (__isolate_free_page(page, order)) {
				set_page_private(page, order);
				nr_isolated = 1 << order;
				cc->nr_freepages += nr_isolated;
				list_add_tail(&page->lru, &cc->freepages);
				count_compact_events(COMPACTISOLATED, nr_isolated);
			} else {
				/* If isolation fails, abort the search */
				order = cc->search_order + 1;
				page = NULL;
			}
		}

		spin_unlock_irqrestore(&cc->zone->lock, flags);

		/*
		 * Smaller scan on next order so the total scan ig related
		 * to freelist_scan_limit.
		 */
		if (order_scanned >= limit)
			limit = min(1U, limit >> 1);
	}

	if (!page) {
		cc->fast_search_fail++;
		if (scan_start) {
			/*
			 * Use the highest PFN found above min. If one was
			 * not found, be pessemistic for direct compaction
			 * and use the min mark.
			 */
			if (highest) {
				page = pfn_to_page(highest);
				cc->free_pfn = highest;
			} else {
				if (cc->direct_compaction) {
					page = pfn_to_page(min_pfn);
					cc->free_pfn = min_pfn;
				}
			}
		}
	}

	if (highest && highest >= cc->zone->compact_cached_free_pfn) {
		highest -= pageblock_nr_pages;
		cc->zone->compact_cached_free_pfn = highest;
	}

	cc->total_free_scanned += nr_scanned;
	if (!page)
		return cc->free_pfn;

	low_pfn = page_to_pfn(page);
	fast_isolate_around(cc, low_pfn, nr_isolated);
	return low_pfn;
}

/*
 * Based on information in the current compact_control, find blocks
 * suitable for isolating free pages from and then isolate them.
 */
static void isolate_freepages(struct compact_control *cc)
{
	struct zone *zone = cc->zone;
	struct page *page;
	unsigned long block_start_pfn;	/* start of current pageblock */
	unsigned long isolate_start_pfn; /* exact pfn we start at */
	unsigned long block_end_pfn;	/* end of current pageblock */
	unsigned long low_pfn;	     /* lowest pfn scanner is able to scan */
	struct list_head *freelist = &cc->freepages;
	unsigned int stride;

	/* Try a small search of the free lists for a candidate */
	isolate_start_pfn = fast_isolate_freepages(cc);
	if (cc->nr_freepages)
		goto splitmap;

	/*
	 * Initialise the free scanner. The starting point is where we last
	 * successfully isolated from, zone-cached value, or the end of the
	 * zone when isolating for the first time. For looping we also need
	 * this pfn aligned down to the pageblock boundary, because we do
	 * block_start_pfn -= pageblock_nr_pages in the for loop.
	 * For ending point, take care when isolating in last pageblock of a
	 * a zone which ends in the middle of a pageblock.
	 * The low boundary is the end of the pageblock the migration scanner
	 * is using.
	 */
	isolate_start_pfn = cc->free_pfn;
	block_start_pfn = pageblock_start_pfn(isolate_start_pfn);
	block_end_pfn = min(block_start_pfn + pageblock_nr_pages,
						zone_end_pfn(zone));
	low_pfn = pageblock_end_pfn(cc->migrate_pfn);
	stride = cc->mode == MIGRATE_ASYNC ? COMPACT_CLUSTER_MAX : 1;

	/*
	 * Isolate free pages until enough are available to migrate the
	 * pages on cc->migratepages. We stop searching if the migrate
	 * and free page scanners meet or enough free pages are isolated.
	 */
	for (; block_start_pfn >= low_pfn;
				block_end_pfn = block_start_pfn,
				block_start_pfn -= pageblock_nr_pages,
				isolate_start_pfn = block_start_pfn) {
		unsigned long nr_isolated;

		/*
		 * This can iterate a massively long zone without finding any
		 * suitable migration targets, so periodically check resched.
		 */
		if (!(block_start_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages)))
			cond_resched();

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
		if (!page)
			continue;

		/* Check the block is suitable for migration */
		if (!suitable_migration_target(cc, page))
			continue;

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;

		/* Found a block suitable for isolating free pages from. */
		nr_isolated = isolate_freepages_block(cc, &isolate_start_pfn,
					block_end_pfn, freelist, stride, false);

		/* Update the skip hint if the full pageblock was scanned */
		if (isolate_start_pfn == block_end_pfn)
			update_pageblock_skip(cc, page, block_start_pfn);

		/* Are enough freepages isolated? */
		if (cc->nr_freepages >= cc->nr_migratepages) {
			if (isolate_start_pfn >= block_end_pfn) {
				/*
				 * Restart at previous pageblock if more
				 * freepages can be isolated next time.
				 */
				isolate_start_pfn =
					block_start_pfn - pageblock_nr_pages;
			}
			break;
		} else if (isolate_start_pfn < block_end_pfn) {
			/*
			 * If isolation failed early, do not continue
			 * needlessly.
			 */
			break;
		}

		/* Adjust stride depending on isolation */
		if (nr_isolated) {
			stride = 1;
			continue;
		}
		stride = min_t(unsigned int, COMPACT_CLUSTER_MAX, stride << 1);
	}

	/*
	 * Record where the free scanner will restart next time. Either we
	 * broke from the loop and set isolate_start_pfn based on the last
	 * call to isolate_freepages_block(), or we met the migration scanner
	 * and the loop terminated due to isolate_start_pfn < low_pfn
	 */
	cc->free_pfn = isolate_start_pfn;

splitmap:
	/* __isolate_free_page() does not map the pages */
	split_map_pages(freelist);
}

/*
 * This is a migrate-callback that "allocates" freepages by taking pages
 * from the isolated freelists in the block we are migrating to.
 */
static struct page *compaction_alloc(struct page *migratepage,
					unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;
	struct page *freepage;

	if (list_empty(&cc->freepages)) {
		isolate_freepages(cc);

		if (list_empty(&cc->freepages))
			return NULL;
	}

	freepage = list_entry(cc->freepages.next, struct page, lru);
	list_del(&freepage->lru);
	cc->nr_freepages--;

	return freepage;
}

/*
 * This is a migrate-callback that "frees" freepages back to the isolated
 * freelist.  All pages on the freelist are from the same zone, so there is no
 * special handling needed for NUMA.
 */
static void compaction_free(struct page *page, unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;

	list_add(&page->lru, &cc->freepages);
	cc->nr_freepages++;
}

/* possible outcome of isolate_migratepages */
typedef enum {
	ISOLATE_ABORT,		/* Abort compaction now */
	ISOLATE_NONE,		/* No pages isolated, continue scanning */
	ISOLATE_SUCCESS,	/* Pages isolated, migrate */
} isolate_migrate_t;

/*
 * Allow userspace to control policy on scanning the unevictable LRU for
 * compactable pages.
 */
int sysctl_compact_unevictable_allowed __read_mostly = 1;

static inline void
update_fast_start_pfn(struct compact_control *cc, unsigned long pfn)
{
	if (cc->fast_start_pfn == ULONG_MAX)
		return;

	if (!cc->fast_start_pfn)
		cc->fast_start_pfn = pfn;

	cc->fast_start_pfn = min(cc->fast_start_pfn, pfn);
}

static inline unsigned long
reinit_migrate_pfn(struct compact_control *cc)
{
	if (!cc->fast_start_pfn || cc->fast_start_pfn == ULONG_MAX)
		return cc->migrate_pfn;

	cc->migrate_pfn = cc->fast_start_pfn;
	cc->fast_start_pfn = ULONG_MAX;

	return cc->migrate_pfn;
}

/*
 * pageblock 이 free 되기 전에 migration이 필요한 페이지 수를 감소시키기 위해
 * 이미 약간의 free pages를 가진 migration source에 대한 free lists를
 * 간단히 찾는다
 */
static unsigned long fast_find_migrateblock(struct compact_control *cc)
{
	unsigned int limit = freelist_scan_limit(cc);
	unsigned int nr_scanned = 0;
	unsigned long distance;
	unsigned long pfn = cc->migrate_pfn;
	unsigned long high_pfn;
	int order;

	/* 빠른 검색에서 반복을 피하기 위해 Skip hints 가 사용된다. */
	if (cc->ignore_skip_hint)
		return pfn;

	/*
	 * migrate_pfn이 영역의 시작이나 페이지 블록의 시작에 있지 않은 경우
	 * COMPACT_CLUSTER_MAX로 인해 다시 시작된 이전 스캔의 연속이라고
	 * 가정합니다.
	 */
	if (pfn != cc->zone->zone_start_pfn && pfn != pageblock_start_pfn(pfn))
		return pfn;

	/*
	 * 작은 주문의 경우 마이그레이션 할 페이지 수가 상대적으로 적어야하며
	 * 작은 할당을 위해 큰 블록을 확보하는 것이 반드시 정당화되지는 않으므로
	 * 선형 스캔만 하라.
	 */
	if (cc->order <= PAGE_ALLOC_COSTLY_ORDER)
		return pfn;

	/*
	 * 할당을 위한 MOVABLE 페이지블록을 신속하게 지우기 위해
	 * kcompactd 와 movable 페이지를 위한 직접 요구만 허락한다.
	 * 이것은 unmovable/reclaimable 을 위한 작은 할당에서 큰 movable
	 * 페이지블록이 해제되는 위험을 줄인다.
	 */
	if (cc->direct_compaction && cc->migratetype != MIGRATE_MOVABLE)
		return pfn;

	/*
	 * migration 스캐너 시작시 검색 공간 중간의 전반부에서 페이지블록을
	 * 선택한다. 그렇지 않을 경우 migration 대상이 후에 소스 가 될
	 * 가능성을 줄이기 위해 처음 8분의 1내 페이지블록을 선택한다.
	 */
	distance = (cc->free_pfn - cc->migrate_pfn) >> 1;
	if (cc->migrate_pfn != cc->zone->zone_start_pfn)
		distance >>= 2;
	high_pfn = pageblock_start_pfn(cc->migrate_pfn + distance);

	for (order = cc->order - 1;
	     order >= PAGE_ALLOC_COSTLY_ORDER && pfn == cc->migrate_pfn && nr_scanned < limit;
	     order--) {
		struct free_area *area = &cc->zone->free_area[order];
		struct list_head *freelist;
		unsigned long flags;
		struct page *freepage;

		if (!area->nr_free)
			continue;

		spin_lock_irqsave(&cc->zone->lock, flags);
		freelist = &area->free_list[MIGRATE_MOVABLE];
		list_for_each_entry(freepage, freelist, lru) {
			unsigned long free_pfn;

			nr_scanned++;
			free_pfn = page_to_pfn(freepage);
			if (free_pfn < high_pfn) {
				/*
				 * 최근 건너뛰었다면 피하라. 이상적으로는 꼬리로
				 * 이동하나 목록의 안전한 반복조차도 항목이 재정렬
				 * 되지 않고 삭제되었다고 가정한다
				 */
				if (get_pageblock_skip(freepage)) {
					if (list_is_last(freelist, &freepage->lru))
						break;

					continue;
				}

				/* 다음 검색에서 최근 페이지를 건너 뛰도록 재정렬 */
				move_freelist_tail(freelist, freepage);

				update_fast_start_pfn(cc, free_pfn);
				pfn = pageblock_start_pfn(free_pfn);
				cc->fast_search_fail = 0;
				set_pageblock_skip(freepage);
				break;
			}

			if (nr_scanned >= limit) {
				cc->fast_search_fail++;
				move_freelist_tail(freelist, freepage);
				break;
			}
		}
		spin_unlock_irqrestore(&cc->zone->lock, flags);
	}

	cc->total_migrate_scanned += nr_scanned;

	/*
	 * If fast scanning failed then use a cached entry for a page block
	 * that had free pages as the basis for starting a linear scan.
	 */
	if (pfn == cc->migrate_pfn)
		pfn = reinit_migrate_pfn(cc);

	return pfn;
}

/*
 * compact_control 내의 마이그레이션 스캐너 pfn이 가리키는 블록에서
 * 시작하여 첫 번째 적합한 블록에서 마이그레이션 할 수있는 모든 페이지를
 * 분리합니다.
 */
static isolate_migrate_t isolate_migratepages(struct zone *zone,
					struct compact_control *cc)
{
	unsigned long block_start_pfn;
	unsigned long block_end_pfn;
	unsigned long low_pfn;
	struct page *page;
	const isolate_mode_t isolate_mode =
		(sysctl_compact_unevictable_allowed ? ISOLATE_UNEVICTABLE : 0) |
		(cc->mode != MIGRATE_SYNC ? ISOLATE_ASYNC_MIGRATE : 0);
	bool fast_find_block;

	/*
	 * 마지막으로 멈춘 지점 또는 compact_zone() 함수에 의해 초기화 된
	 * zone의 시작점에서 시작한다. 첫번째 실패는 선형 scanning 을 위한
	 * 시작점으로서 가장 아래 PFN을 사용한다.(?)
	 */
	low_pfn = fast_find_migrateblock(cc);
	block_start_pfn = pageblock_start_pfn(low_pfn);
	if (block_start_pfn < zone->zone_start_pfn)
		block_start_pfn = zone->zone_start_pfn;

	/*
	 * fast_find_migrateblock 함수는 아래의 isolation_suitable 함수의 검사를
	 * 피하기 위해 건너 뛴 페이지 블록을 표시하므로 빠른 검색이 성공했는지
	 * 확인합니다.(?)
	 */
	fast_find_block = low_pfn != cc->migrate_pfn && !cc->fast_search_fail;

	/* Only scan within a pageblock boundary */
	block_end_pfn = pageblock_end_pfn(low_pfn);

	/*
	 * 첫번째 적합 요소를 찾을 때까지 모든 페이지블록을 반복하라.
	 * free scaner를 침범하지 마라.
	 */
	for (; block_end_pfn <= cc->free_pfn;
			fast_find_block = false,
			low_pfn = block_end_pfn,
			block_start_pfn = block_end_pfn,
			block_end_pfn += pageblock_nr_pages) {

		/*
		 * 부적합한 페이지 블록이 매우 긴 zone을 반복할수 있으므로
		 * 스케쥴링이 필요한지 주기적으로 확인하라.
		 */
		// 32번에 한번꼴
		if (!(low_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages)))
			cond_resched();

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
		if (!page)
			continue;

		/*
		 * isolation 이 최근에 실패하였다면 재시도 하지 마세요.
		 * 페이지블록을 한번만 확인하세요. COMPACT_CLUSTER_MAX 는 페이지블록을
		 * 여러번 방문하도록 한다. 건너뛰기는 그것을 "skip"으로 만들기 전에
		 * 검사되었다고 가정하므로 다른 compaction 객체들은 같은 블록을
		 * 스캔하지 않는다.
		 */
		if (IS_ALIGNED(low_pfn, pageblock_nr_pages) &&
		    !fast_find_block && !isolation_suitable(cc, page))
			continue;

		/*
		 * 비동기 compaction의 경우 또한 huge 페이지가 없는 MOVABLE 블록
		 * 에서만 스캔한다. 비동기 compaction 은 최소량의 작업으로 할당을
		 * 만족시키는지를 알아보는데 낙관적이다. 캐쉬된 PFN은 모든 남아있는
		 * 소스와 타겟사이의 블록이 적당하지 않고 compaction 스캐너가
		 * 충족하지 못할 수 있으므로 업데이트 된다.
		 */
		if (!suitable_migration_source(cc, page)) {
			update_cached_migrate(cc, block_end_pfn);
			continue;
		}

		/* Perform the isolation */
		low_pfn = isolate_migratepages_block(cc, low_pfn,
						block_end_pfn, isolate_mode);

		if (!low_pfn)
			return ISOLATE_ABORT;

		/*
		 * Either we isolated something and proceed with migration. Or
		 * we failed and compact_zone should decide if we should
		 * continue or not.
		 */
		break;
	}

	/* Record where migration scanner will be restarted. */
	cc->migrate_pfn = low_pfn;

	return cc->nr_migratepages ? ISOLATE_SUCCESS : ISOLATE_NONE;
}

/*
 * order == -1 is expected when compacting via
 * /proc/sys/vm/compact_memory
 */
static inline bool is_via_compact_memory(int order)
{
	return order == -1;
}

static enum compact_result __compact_finished(struct compact_control *cc)
{
	unsigned int order;
	const int migratetype = cc->migratetype;
	int ret;

	/* migrate 와 free 스캐너가 만나면 compaction 실행이 완료된다. */
	if (compact_scanners_met(cc)) {
		/* 다음 compaction  다시 시작 */
		reset_cached_positions(cc->zone);

		/*
		 * PG_migrate_skip 정보는 휴면상태가 될 때 kswapd에 의해 지워
		 * 져야 한다고 표시. kcompactd는 지움 결정이 직접적으로 할당 요구에
		 * 기초하여야 하므로 flag를 설정하지 못한다.
		 */
		if (cc->direct_compaction)
			cc->zone->compact_blockskip_flush = true;

		if (cc->whole_zone)
			return COMPACT_COMPLETE;
		else
			return COMPACT_PARTIAL_SKIPPED;
	}

	if (is_via_compact_memory(cc->order))
		return COMPACT_CONTINUE;

	/*
	 * 향후 대체모드로의 가능성을 줄이리면 항상 페이지블록 스캔을 완료하라.
	 * 이것은 migration 소스가 unmovable/reclaimable 일때 특히 중요하나
	 * 특별한 경우는 가치가 없다.
	 */
	if (!IS_ALIGNED(cc->migrate_pfn, pageblock_nr_pages))
		return COMPACT_CONTINUE;

	/* Direct compactor: Is a suitable page free? */
	ret = COMPACT_NO_SUITABLE_PAGE;
	for (order = cc->order; order < MAX_ORDER; order++) {
		struct free_area *area = &cc->zone->free_area[order];
		bool can_steal;

		/* 페이지가 올바른 migratetype의 free 페이지 이면 끝 */
		if (!list_empty(&area->free_list[migratetype]))
			return COMPACT_SUCCESS;

#ifdef CONFIG_CMA
		/* MIGRATE_MOVABLE 은 MIGRATE_CMA의 대체모드가 될 수 있다 */
		if (migratetype == MIGRATE_MOVABLE &&
			!list_empty(&area->free_list[MIGRATE_CMA]))
			return COMPACT_SUCCESS;
#endif
		/*
		 * 할당이 다른 migratetype 버디 목록에서 freepage를 가져올 수
		 * 있다면 작업은 완료되었다.
		 */
		if (find_suitable_fallback(area, order, migratetype,
						true, &can_steal) != -1) {

			/* movable 페이지는 어떤 페이지블록이라도 괜찮다 */
			if (migratetype == MIGRATE_MOVABLE)
				return COMPACT_SUCCESS;

			/*
			 * non-movable 할당을 위해 가져오는 중이다.  가능한 free
			 * 하고 바로 다른 페이지블록을 가져오지 않아도 되도록 먼저
			 * 현재 페이지블록 compaction이 완료 되었는지 확인하라.
			 * 비동기 compaction은 같은 migratetype의 페이지블록에
			 * 동작하므로 이것은 동기 compaction에만 적용된다
			 */
			if (cc->mode == MIGRATE_ASYNC ||
					IS_ALIGNED(cc->migrate_pfn,
							pageblock_nr_pages)) {
				return COMPACT_SUCCESS;
			}

			ret = COMPACT_CONTINUE;
			break;
		}
	}

	if (cc->contended || fatal_signal_pending(current))
		ret = COMPACT_CONTENDED;

	return ret;
}

static enum compact_result compact_finished(struct compact_control *cc)
{
	int ret;

	ret = __compact_finished(cc);
	trace_mm_compaction_finished(cc->zone, cc->order, ret);
	if (ret == COMPACT_NO_SUITABLE_PAGE)
		ret = COMPACT_CONTINUE;

	return ret;
}

/*
 * compaction_suitable: Is this suitable to run compaction on this zone now?
 * Returns
 *   COMPACT_SKIPPED  - If there are too few free pages for compaction
 *   COMPACT_SUCCESS  - If the allocation would succeed without compaction
 *   COMPACT_CONTINUE - If compaction should run now
 */
static enum compact_result __compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx,
					unsigned long wmark_target)
{
	unsigned long watermark;

	if (is_via_compact_memory(order))
		return COMPACT_CONTINUE;

	watermark = wmark_pages(zone, alloc_flags & ALLOC_WMARK_MASK);
	/*
	 * If watermarks for high-order allocation are already met, there
	 * should be no need for compaction at all.
	 */
	if (zone_watermark_ok(zone, order, watermark, classzone_idx,
								alloc_flags))
		return COMPACT_SUCCESS;

	/*
	 * Watermarks for order-0 must be met for compaction to be able to
	 * isolate free pages for migration targets. This means that the
	 * watermark and alloc_flags have to match, or be more pessimistic than
	 * the check in __isolate_free_page(). We don't use the direct
	 * compactor's alloc_flags, as they are not relevant for freepage
	 * isolation. We however do use the direct compactor's classzone_idx to
	 * skip over zones where lowmem reserves would prevent allocation even
	 * if compaction succeeds.
	 * For costly orders, we require low watermark instead of min for
	 * compaction to proceed to increase its chances.
	 * ALLOC_CMA is used, as pages in CMA pageblocks are considered
	 * suitable migration targets
	 */
	watermark = (order > PAGE_ALLOC_COSTLY_ORDER) ?
				low_wmark_pages(zone) : min_wmark_pages(zone);
	// compaction시 freepages에 복사가 이루어 지고 아래에서 0 order로
	// watermark 검사를 하므로 order 2배 정도의 격차를 더한다
	watermark += compact_gap(order);
	if (!__zone_watermark_ok(zone, 0, watermark, classzone_idx,
						ALLOC_CMA, wmark_target))
		return COMPACT_SKIPPED;

	return COMPACT_CONTINUE;
}

enum compact_result compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx)
{
	enum compact_result ret;
	int fragindex;

	ret = __compaction_suitable(zone, order, alloc_flags, classzone_idx,
				    zone_page_state(zone, NR_FREE_PAGES));
	/*
	 * fragmentation index determines if allocation failures are due to
	 * low memory or external fragmentation
	 *
	 * index of -1000 would imply allocations might succeed depending on
	 * watermarks, but we already failed the high-order watermark check
	 * index towards 0 implies failure is due to lack of memory
	 * index towards 1000 implies failure is due to fragmentation
	 *
	 * Only compact if a failure would be due to fragmentation. Also
	 * ignore fragindex for non-costly orders where the alternative to
	 * a successful reclaim/compaction is OOM. Fragindex and the
	 * vm.extfrag_threshold sysctl is meant as a heuristic to prevent
	 * excessive compaction for costly orders, but it should not be at the
	 * expense of system stability.
	 */
	// compation 진행해야할 상황에서 order가 4이상 이면 단편화 계수를
	// 구해서 0에서 500 이하이면 단편화 되지 않아서 compaction 효과가
	// 없다고 보고 skip 하도록 한다.
	if (ret == COMPACT_CONTINUE && (order > PAGE_ALLOC_COSTLY_ORDER)) {
		fragindex = fragmentation_index(zone, order);
		if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold)
			ret = COMPACT_NOT_SUITABLE_ZONE;
	}

	trace_mm_compaction_suitable(zone, order, ret);
	if (ret == COMPACT_NOT_SUITABLE_ZONE)
		ret = COMPACT_SKIPPED;

	return ret;
}

bool compaction_zonelist_suitable(struct alloc_context *ac, int order,
		int alloc_flags)
{
	struct zone *zone;
	struct zoneref *z;

	/*
	 * Make sure at least one zone would pass __compaction_suitable if we continue
	 * retrying the reclaim.
	 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
					ac->nodemask) {
		unsigned long available;
		enum compact_result compact_result;

		/*
		 * Do not consider all the reclaimable memory because we do not
		 * want to trash just for a single high order allocation which
		 * is even not guaranteed to appear even if __compaction_suitable
		 * is happy about the watermark check.
		 */
		available = zone_reclaimable_pages(zone) / order;
		available += zone_page_state_snapshot(zone, NR_FREE_PAGES);
		compact_result = __compaction_suitable(zone, order, alloc_flags,
				ac_classzone_idx(ac), available);
		if (compact_result != COMPACT_SKIPPED)
			return true;
	}

	return false;
}

static enum compact_result
compact_zone(struct compact_control *cc, struct capture_control *capc)
{
	enum compact_result ret;
	unsigned long start_pfn = cc->zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(cc->zone);
	unsigned long last_migrated_pfn;
	const bool sync = cc->mode != MIGRATE_ASYNC;
	bool update_cached;

	cc->migratetype = gfpflags_to_migratetype(cc->gfp_mask);
	ret = compaction_suitable(cc->zone, cc->order, cc->alloc_flags,
							cc->classzone_idx);
	/* Compaction 이 실패한 것 같다. */
	if (ret == COMPACT_SUCCESS || ret == COMPACT_SKIPPED)
		return ret;

	/* compaction_suitable 함수가 예상치 못한 것을 반환함 */
	VM_BUG_ON(ret != COMPACT_CONTINUE);

	/*
	 * 최근에 실패하고 compaction이 지연된후 재시도되는 경우
	 * 페이지블록 skip 비트를 지워라.
	 */
	 /* compaction이 64번 이상 실패한 경우 다시 시작하기 위해 */
	 /* pageblock 의 skip 비트를 모두 clear한다. */
	if (compaction_restarting(cc->zone, cc->order))
		__reset_isolation_suitable(cc->zone);

	/*
	 * zone의 끝으로 모든 movable 페이지를 옮기도록 설정. 스캐너가 시작해야할
	 * 사용된 캐시 정보(명백하게 전체 zone을 compact하기 원하지 않는다면),
	 * 그러나 값이 zone경계 안에 있도록 초기화 되었는지 검사하라.
	 */
	cc->fast_start_pfn = 0;
	if (cc->whole_zone) {
		cc->migrate_pfn = start_pfn;
		cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
	} else {
		cc->migrate_pfn = cc->zone->compact_cached_migrate_pfn[sync];
		cc->free_pfn = cc->zone->compact_cached_free_pfn;
		if (cc->free_pfn < start_pfn || cc->free_pfn >= end_pfn) {
			cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
			cc->zone->compact_cached_free_pfn = cc->free_pfn;
		}
		if (cc->migrate_pfn < start_pfn || cc->migrate_pfn >= end_pfn) {
			cc->migrate_pfn = start_pfn;
			cc->zone->compact_cached_migrate_pfn[0] = cc->migrate_pfn;
			cc->zone->compact_cached_migrate_pfn[1] = cc->migrate_pfn;
		}

		if (cc->migrate_pfn <= cc->zone->compact_init_migrate_pfn)
			cc->whole_zone = true;
	}

	last_migrated_pfn = 0;

	/*
	 * Migrate는 일부 migration 이 비동기 모드에서 실패할 수 있다는 기준으로
	 * 동기와 비동기 migration을 위한 별도의 캐쉬된 PFN을 가진다.
	 * 그러나 만약 캐쉬된 PFN이 일치하고 isolation 후보가 없어서 페이지블록을
	 * 건너뛴다면 동기화 상태는 중요하지 않다.
	 * isolation 후보가 발견되기 전까지 같은 블록이 재방문 되는 것을
	 * 피하기 위해 동기화 상태로 캐쉬된 PFN 을 유지하라.
	 */
	update_cached = !sync &&
		cc->zone->compact_cached_migrate_pfn[0] == cc->zone->compact_cached_migrate_pfn[1];

	trace_mm_compaction_begin(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync);

	migrate_prep_local();

	while ((ret = compact_finished(cc)) == COMPACT_CONTINUE) {
		int err;
		unsigned long start_pfn = cc->migrate_pfn;

		/*
		 * 만약 페이지가 isolated 될 수 없는 페이지
		 * (비동기 모드의 dirty/writeback)나 페이지블록이 지워지기 전에
		 * migrated 페이지가 할당될 수 있는 여러 rescan을 피하라.
		 * 첫번째 migration을 위해 전체 페이지블록을 캡쳐한다. 실패하면
		 * skip으로 표시되고 스캔이 정상으로 진행된다.
		 */
		cc->rescan = false;
		if (pageblock_start_pfn(last_migrated_pfn) ==
		    pageblock_start_pfn(start_pfn)) {
			cc->rescan = true;
		}

		switch (isolate_migratepages(cc->zone, cc)) {
		case ISOLATE_ABORT:
			ret = COMPACT_CONTENDED;
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			last_migrated_pfn = 0;
			goto out;
		case ISOLATE_NONE:
			if (update_cached) {
				cc->zone->compact_cached_migrate_pfn[1] =
					cc->zone->compact_cached_migrate_pfn[0];
			}

			/*
			 * We haven't isolated and migrated anything, but
			 * there might still be unflushed migrations from
			 * previous cc->order aligned block.
			 */
			goto check_drain;
		case ISOLATE_SUCCESS:
			update_cached = false;
			last_migrated_pfn = start_pfn;
			;
		}

		err = migrate_pages(&cc->migratepages, compaction_alloc,
				compaction_free, (unsigned long)cc, cc->mode,
				MR_COMPACTION);

		trace_mm_compaction_migratepages(cc->nr_migratepages, err,
							&cc->migratepages);

		/* All pages were either migrated or will be released */
		cc->nr_migratepages = 0;
		if (err) {
			putback_movable_pages(&cc->migratepages);
			/*
			 * migrate_pages() may return -ENOMEM when scanners meet
			 * and we want compact_finished() to detect it
			 */
			if (err == -ENOMEM && !compact_scanners_met(cc)) {
				ret = COMPACT_CONTENDED;
				goto out;
			}
			/*
			 * We failed to migrate at least one page in the current
			 * order-aligned block, so skip the rest of it.
			 */
			if (cc->direct_compaction &&
						(cc->mode == MIGRATE_ASYNC)) {
				cc->migrate_pfn = block_end_pfn(
						cc->migrate_pfn - 1, cc->order);
				/* Draining pcplists is useless in this case */
				last_migrated_pfn = 0;
			}
		}

check_drain:
		/*
		 * migration 스캐너가 migrate한 이전 cc->order 정렬 블록으로부터
		 * 멀어 졌나? 그렇다면 병합하고 compact_finished 함수가 즉시 할당에
		 * 성공했음을수 감지 할 수 있게 free된 페이지를 비워라.
		 */
		if (cc->order > 0 && last_migrated_pfn) {
			int cpu;
			unsigned long current_block_start =
				block_start_pfn(cc->migrate_pfn, cc->order);

			if (last_migrated_pfn < current_block_start) {
				cpu = get_cpu();
				lru_add_drain_cpu(cpu);
				drain_local_pages(cc->zone);
				put_cpu();
				/* No more flushing until we migrate again */
				last_migrated_pfn = 0;
			}
		}

		/* 페이지를 획득했다면 중지하라 */
		if (capc && capc->page) {
			ret = COMPACT_SUCCESS;
			break;
		}
	}

out:
	/*
	 * Release free pages and update where the free scanner should restart,
	 * so we don't leave any returned pages behind in the next attempt.
	 */
	if (cc->nr_freepages > 0) {
		unsigned long free_pfn = release_freepages(&cc->freepages);

		cc->nr_freepages = 0;
		VM_BUG_ON(free_pfn == 0);
		/* The cached pfn is always the first in a pageblock */
		free_pfn = pageblock_start_pfn(free_pfn);
		/*
		 * Only go back, not forward. The cached pfn might have been
		 * already reset to zone end in compact_finished()
		 */
		if (free_pfn > cc->zone->compact_cached_free_pfn)
			cc->zone->compact_cached_free_pfn = free_pfn;
	}

	count_compact_events(COMPACTMIGRATE_SCANNED, cc->total_migrate_scanned);
	count_compact_events(COMPACTFREE_SCANNED, cc->total_free_scanned);

	trace_mm_compaction_end(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync, ret);

	return ret;
}

static enum compact_result compact_zone_order(struct zone *zone, int order,
		gfp_t gfp_mask, enum compact_priority prio,
		unsigned int alloc_flags, int classzone_idx,
		struct page **capture)
{
	enum compact_result ret;
	struct compact_control cc = {
		.nr_freepages = 0,
		.nr_migratepages = 0,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.order = order,
		.search_order = order,
		.gfp_mask = gfp_mask,
		.zone = zone,
		.mode = (prio == COMPACT_PRIO_ASYNC) ?
					MIGRATE_ASYNC :	MIGRATE_SYNC_LIGHT,
		.alloc_flags = alloc_flags,
		.classzone_idx = classzone_idx,
		.direct_compaction = true,
		.whole_zone = (prio == MIN_COMPACT_PRIORITY),
		.ignore_skip_hint = (prio == MIN_COMPACT_PRIORITY),
		.ignore_block_suitable = (prio == MIN_COMPACT_PRIORITY)
	};
	struct capture_control capc = {
		.cc = &cc,
		.page = NULL,
	};

	if (capture)
		current->capture_control = &capc;
	INIT_LIST_HEAD(&cc.freepages);
	INIT_LIST_HEAD(&cc.migratepages);

	ret = compact_zone(&cc, &capc);

	VM_BUG_ON(!list_empty(&cc.freepages));
	VM_BUG_ON(!list_empty(&cc.migratepages));

	*capture = capc.page;
	current->capture_control = NULL;

	return ret;
}

int sysctl_extfrag_threshold = 500;

/**
 * try_to_compact_pages - 상위 order 할당을 만족하기위한 직접 compact
 * @gfp_mask: The GFP mask of the current allocation
 * @order: The order of the current allocation
 * @alloc_flags: The allocation flags of the current allocation
 * @ac: The context of current allocation
 * @prio: 직접 compaction이 성공하기 위해 시도해야하는 강도를 설정
 *
 * 직접 페이지 compaction을 위한 주 진입점이다.
 */
enum compact_result try_to_compact_pages(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		enum compact_priority prio, struct page **capture)
{
	int may_perform_io = gfp_mask & __GFP_IO;
	struct zoneref *z;
	struct zone *zone;
	enum compact_result rc = COMPACT_SKIPPED;

	/*
	 * Check if the GFP flags allow compaction - GFP_NOIO is really
	 * tricky context because the migration might require IO
	 */
	if (!may_perform_io)
		return COMPACT_SKIPPED;

	trace_mm_compaction_try_to_compact_pages(order, gfp_mask, prio);

	/* 목록의 각 zone compact */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
								ac->nodemask) {
		enum compact_result status;

		if (prio > MIN_COMPACT_PRIORITY
					&& compaction_deferred(zone, order)) {
			rc = max_t(enum compact_result, COMPACT_DEFERRED, rc);
			continue;
		}

		status = compact_zone_order(zone, order, gfp_mask, prio,
				alloc_flags, ac_classzone_idx(ac), capture);
		rc = max(status, rc);

		/* The allocation should succeed, stop compacting */
		if (status == COMPACT_SUCCESS) {
			/*
			 * We think the allocation will succeed in this zone,
			 * but it is not certain, hence the false. The caller
			 * will repeat this with true if allocation indeed
			 * succeeds in this zone.
			 */
			compaction_defer_reset(zone, order, false);

			break;
		}

		if (prio != COMPACT_PRIO_ASYNC && (status == COMPACT_COMPLETE ||
					status == COMPACT_PARTIAL_SKIPPED))
			/*
			 * We think that allocation won't succeed in this zone
			 * so we defer compaction there. If it ends up
			 * succeeding after all, it will be reset.
			 */
			defer_compaction(zone, order);

		/*
		 * We might have stopped compacting due to need_resched() in
		 * async compaction, or due to a fatal signal detected. In that
		 * case do not try further zones
		 */
		if ((prio == COMPACT_PRIO_ASYNC && need_resched())
					|| fatal_signal_pending(current))
			break;
	}

	return rc;
}


/* Compact all zones within a node */
static void compact_node(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = -1,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.mode = MIGRATE_SYNC,
		.ignore_skip_hint = true,
		.whole_zone = true,
		.gfp_mask = GFP_KERNEL,
	};


	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		cc.nr_freepages = 0;
		cc.nr_migratepages = 0;
		cc.zone = zone;
		INIT_LIST_HEAD(&cc.freepages);
		INIT_LIST_HEAD(&cc.migratepages);

		compact_zone(&cc, NULL);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}
}

/* Compact all nodes in the system */
static void compact_nodes(void)
{
	int nid;

	/* Flush pending updates to the LRU lists */
	lru_add_drain_all();

	for_each_online_node(nid)
		compact_node(nid);
}

/* The written value is actually unused, all memory is compacted */
int sysctl_compact_memory;

/*
 * This is the entry point for compacting all nodes via
 * /proc/sys/vm/compact_memory
 */
int sysctl_compaction_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos)
{
	if (write)
		compact_nodes();

	return 0;
}

#if defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
static ssize_t sysfs_compact_node(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int nid = dev->id;

	if (nid >= 0 && nid < nr_node_ids && node_online(nid)) {
		/* Flush pending updates to the LRU lists */
		lru_add_drain_all();

		compact_node(nid);
	}

	return count;
}
static DEVICE_ATTR(compact, 0200, NULL, sysfs_compact_node);

int compaction_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_compact);
}

void compaction_unregister_node(struct node *node)
{
	return device_remove_file(&node->dev, &dev_attr_compact);
}
#endif /* CONFIG_SYSFS && CONFIG_NUMA */

static inline bool kcompactd_work_requested(pg_data_t *pgdat)
{
	return pgdat->kcompactd_max_order > 0 || kthread_should_stop();
}

static bool kcompactd_node_suitable(pg_data_t *pgdat)
{
	int zoneid;
	struct zone *zone;
	enum zone_type classzone_idx = pgdat->kcompactd_classzone_idx;

	for (zoneid = 0; zoneid <= classzone_idx; zoneid++) {
		zone = &pgdat->node_zones[zoneid];

		if (!populated_zone(zone))
			continue;

		if (compaction_suitable(zone, pgdat->kcompactd_max_order, 0,
					classzone_idx) == COMPACT_CONTINUE)
			return true;
	}

	return false;
}

static void kcompactd_do_work(pg_data_t *pgdat)
{
	/*
	 * With no special task, compact all zones so that a page of requested
	 * order is allocatable.
	 */
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = pgdat->kcompactd_max_order,
		.search_order = pgdat->kcompactd_max_order,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.classzone_idx = pgdat->kcompactd_classzone_idx,
		.mode = MIGRATE_SYNC_LIGHT,
		.ignore_skip_hint = false,
		.gfp_mask = GFP_KERNEL,
	};
	trace_mm_compaction_kcompactd_wake(pgdat->node_id, cc.order,
							cc.classzone_idx);
	count_compact_event(KCOMPACTD_WAKE);

	for (zoneid = 0; zoneid <= cc.classzone_idx; zoneid++) {
		int status;

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		if (compaction_deferred(zone, cc.order))
			continue;

		if (compaction_suitable(zone, cc.order, 0, zoneid) !=
							COMPACT_CONTINUE)
			continue;

		cc.nr_freepages = 0;
		cc.nr_migratepages = 0;
		cc.total_migrate_scanned = 0;
		cc.total_free_scanned = 0;
		cc.zone = zone;
		INIT_LIST_HEAD(&cc.freepages);
		INIT_LIST_HEAD(&cc.migratepages);

		if (kthread_should_stop())
			return;
		status = compact_zone(&cc, NULL);

		if (status == COMPACT_SUCCESS) {
			compaction_defer_reset(zone, cc.order, false);
		} else if (status == COMPACT_PARTIAL_SKIPPED || status == COMPACT_COMPLETE) {
			/*
			 * Buddy pages may become stranded on pcps that could
			 * otherwise coalesce on the zone's free area for
			 * order >= cc.order.  This is ratelimited by the
			 * upcoming deferral.
			 */
			drain_all_pages(zone);

			/*
			 * We use sync migration mode here, so we defer like
			 * sync direct compaction does.
			 */
			defer_compaction(zone, cc.order);
		}

		count_compact_events(KCOMPACTD_MIGRATE_SCANNED,
				     cc.total_migrate_scanned);
		count_compact_events(KCOMPACTD_FREE_SCANNED,
				     cc.total_free_scanned);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}

	/*
	 * Regardless of success, we are done until woken up next. But remember
	 * the requested order/classzone_idx in case it was higher/tighter than
	 * our current ones
	 */
	if (pgdat->kcompactd_max_order <= cc.order)
		pgdat->kcompactd_max_order = 0;
	if (pgdat->kcompactd_classzone_idx >= cc.classzone_idx)
		pgdat->kcompactd_classzone_idx = pgdat->nr_zones - 1;
}

void wakeup_kcompactd(pg_data_t *pgdat, int order, int classzone_idx)
{
	if (!order)
		return;

	if (pgdat->kcompactd_max_order < order)
		pgdat->kcompactd_max_order = order;

	if (pgdat->kcompactd_classzone_idx > classzone_idx)
		pgdat->kcompactd_classzone_idx = classzone_idx;

	/*
	 * Pairs with implicit barrier in wait_event_freezable()
	 * such that wakeups are not missed.
	 */
	if (!wq_has_sleeper(&pgdat->kcompactd_wait))
		return;

	if (!kcompactd_node_suitable(pgdat))
		return;

	trace_mm_compaction_wakeup_kcompactd(pgdat->node_id, order,
							classzone_idx);
	wake_up_interruptible(&pgdat->kcompactd_wait);
}

/*
 * The background compaction daemon, started as a kernel thread
 * from the init process.
 */
static int kcompactd(void *p)
{
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;

	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	set_freezable();

	pgdat->kcompactd_max_order = 0;
	pgdat->kcompactd_classzone_idx = pgdat->nr_zones - 1;

	while (!kthread_should_stop()) {
		unsigned long pflags;

		trace_mm_compaction_kcompactd_sleep(pgdat->node_id);
		wait_event_freezable(pgdat->kcompactd_wait,
				kcompactd_work_requested(pgdat));

		psi_memstall_enter(&pflags);
		kcompactd_do_work(pgdat);
		psi_memstall_leave(&pflags);
	}

	return 0;
}

/*
 * This kcompactd start function will be called by init and node-hot-add.
 * On node-hot-add, kcompactd will moved to proper cpus if cpus are hot-added.
 */
int kcompactd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kcompactd)
		return 0;

	pgdat->kcompactd = kthread_run(kcompactd, pgdat, "kcompactd%d", nid);
	if (IS_ERR(pgdat->kcompactd)) {
		pr_err("Failed to start kcompactd on node %d\n", nid);
		ret = PTR_ERR(pgdat->kcompactd);
		pgdat->kcompactd = NULL;
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined. Caller must
 * hold mem_hotplug_begin/end().
 */
void kcompactd_stop(int nid)
{
	struct task_struct *kcompactd = NODE_DATA(nid)->kcompactd;

	if (kcompactd) {
		kthread_stop(kcompactd);
		NODE_DATA(nid)->kcompactd = NULL;
	}
}

/*
 * It's optimal to keep kcompactd on the same CPUs as their memory, but
 * not required for correctness. So if the last cpu in a node goes
 * away, we get changed to run anywhere: as the first one comes back,
 * restore their cpu bindings.
 */
static int kcompactd_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		const struct cpumask *mask;

		mask = cpumask_of_node(pgdat->node_id);

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
			/* One of our CPUs online: restore mask */
			set_cpus_allowed_ptr(pgdat->kcompactd, mask);
	}
	return 0;
}

static int __init kcompactd_init(void)
{
	int nid;
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/compaction:online",
					kcompactd_cpu_online, NULL);
	if (ret < 0) {
		pr_err("kcompactd: failed to register hotplug callbacks.\n");
		return ret;
	}

	for_each_node_state(nid, N_MEMORY)
		kcompactd_run(nid);
	return 0;
}
subsys_initcall(kcompactd_init)

#endif /* CONFIG_COMPACTION */
