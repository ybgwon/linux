/* SPDX-License-Identifier: GPL-2.0 */
/*
 * page->flags를 조작하고 테스트하기위한 매크로
 */

#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/mmdebug.h>
#ifndef __GENERATING_BOUNDS_H
#include <linux/mm_types.h>
#include <generated/bounds.h>
#endif /* !__GENERATING_BOUNDS_H */

/*
 * 다양한 page->flags 비트:
 *
 * PG_reserved는 특수 페이지에 설정된다. 이런한 페이지의 "페이지 구조체"는 일반적으로
 * 소유자를 제외하고는 건드리지 않아야 한다.(예. dirty 설정)
 * PG_reserved로 표시된 페이지는 다음과 같다.
 * - 커널 이미지의 일부(vDSO 포함) 및 유사 페이지(예. 바이오스, initrd, HW 테이블)
 * - 예약 되었거나 초기 부팅중에 할당된 페이지(페이지 할당자가 초기화되기전)
 *   여기에는 초기 vmemmap, 초기 페이지 테이블, crashkernel, elfcorehdr, 등이
 *   포함된다.(아키텍쳐에 따라 다름)
 *   해제되면 PG_reserved는 지워지고 페이지 할당자에게 제공된다.
 * - 물리 메모리 gap 에 있는 페이지 - IORESOURCE_SYSRAM 는 제외. 이 페이지들을
 *   읽기/쓰기 를 시도하는 것은 좋지 않은 결과를 가져올 지 모른다. 건들지 마라.
 * - zero 페이지
 * - online_page_callback을 통해 제외 되었거나 PG_hwpoison 으로 인해 섹션을
 *   온라인화 할때 페이지 할당자가 추가하지 않은 페이지
 * - kexec/kdump context 에서 할당된 페이지 (로드된 커널 이미지,
 *   제어 페이지, vmcoreinfo)
 * - MMIO/DMA 페이지. 일부 아키텍쳐는 PG_reserved 로 표시되지 않은 페이지는 ioremap을
 *   허용하지 않는다. (이페이지들은 캐싱 전략을 존중하지 않은 누군가에 의해 사용
 *   되고 있을수 있으므로)
 * - 오프라인 섹션의 부분인 페이지 (오프라인 섹션의 페이지 구조체는 처음 온라인 상태일 때
 *   초기화 되므로 신뢰해서는 안된다)
 * - ia64의 MCA 페이지
 * - POWER Firmware Assisted Dump을 위한 CPU 노트를 가진 페이지
 * - 디바이스 메모리(예. PMEM, DAX, HMM)
 *
 * 일부 PG_reserved 페이지는 최대절전모드 이미지에서 제외된다.
 * PG_reserved 는 보통 누구에게도 dumping 이나 swapping 을 방해하지 않고
 * 더이상 remap_pfn_range 가 필요없다. ioremap 이 필요할 수 있다.
 * 따라서 유저 영역에 맵핑된 페이지의 PG_reserved는 zero page, vDSO, MMIO 페이지나
 * 디바이스 메모리를 나타낼 수 있다.
 *
 * PG_private 비트플래그는 페이지캐쉬 페이지에 파일시스템 특정 데이터를 포함하는 경우
 * 설정된다. (보통 page->private 에 있는). 자체 사용을 위한 개인 할당에 사용되어진다.
 *
 * 디스크 I/O의 초기화 중 PG_locked 는 설정된다. 이 비트는 I/O 전에 설정되고
 * writeback이 _starts_ 나 read가 _completes_ 일때 제거된다.
 * PG_writeback은 writeback 시작 전에 설정되고 끝날때 제거된다.
 *
 * PG_locked는 페이지캐쉬에 페이지를 고정하고 파일이 보관되는 동안 파일의 잘림을 막는다.(?)
 *
 * page_waitqueue는 페이지가 잠금 해제 되려고 기다리는 모든 작업의 대기열이다.
 *
 * PG_uptodate는 페이지의 내용이 유효한지를 나타낸다. 디스크 I/O 에러 없이
 * 읽기 완료되면 페이지는 최신상태(uptodate)된다.
 *
 * PG_referenced, PG_reclaim 는 익명(anonymous) 과 file-backed 페이지캐쉬를 위한
 * 페이지 회수를 위해 사용된다. (mm/vmscan.c 참조)
 *
 * PG_error 는 이 페이지에 I/O 에러가 발생했는지를 나타내려고 설정한다.
 *
 * PG_arch_1은 아키텍쳐 특정 페이지 상태 비트이다. 일반 코드는 페이지 캐쉬에 처음
 * 입력될 때 페이지의 이 비트가 지워짐을 보장한다.
 *
 * PG_hwpoison 은 page에 하드웨어 오류가 있고 machine check에서 야기된
 * 올바르지 않은 ECC bits를 가진 데이터를 포함함을 나타낸다. 또다른 machine check를
 * 유발할 수 있으므로 접근은 안전하지 않다. 건드리지 마라.!
 */

/*
 * *_dontuse flag를 사용하지 말라. 매크로를 사용하라. 그렇지 않으면
 * locked- 와 dirty-page accounting 이 깨진다.
 *
 * The page flags field is split into two parts, the main flags area
 * which extends from the low bits upwards, and the fields area which
 * extends from the high bits downwards.
 * page flags 필드는 두부분으로 나눠지는데, 주 flag 영역은 하위 비트에서 상위로
 * 확장하고,  fields 영역은 상위 비트에서 아래로 확장한다.
 *
 *  | FIELD | ... | FLAGS |
 *  N-1           ^       0
 *               (NR_PAGEFLAGS)
 *
 * fields 영역은 fields 매핑 영역, node(for NUMA), SPARSEMEM 섹션을 위해 예약된다.
 * (SPARSEMEM_VMEMMAP이 아닌 SPARSEMEM_EXTREME 같은 섹션아이디를 필요로 하는
 * SPARSEMEM 의 변형을 위함)
 */
enum pageflags {
	PG_locked,		/* Page is locked. Don't touch. */
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_active,
	PG_workingset,
	PG_waiters,		/* Page has waiters, check its waitqueue. Must be bit #7 and in the same byte as "PG_locked" */
	PG_error,
	PG_slab,
	PG_owner_priv_1,	/* Owner use. If pagecache, fs may use*/
	PG_arch_1,
	PG_reserved,
	PG_private,		/* If pagecache, has fs-private data */
	PG_private_2,		/* If pagecache, has fs aux data */
	PG_writeback,		/* Page is under writeback */
	PG_head,		/* A head page */
	PG_mappedtodisk,	/* Has blocks allocated on-disk */
	PG_reclaim,		/* To be reclaimed asap */
	PG_swapbacked,		/* Page is backed by RAM/swap */
	PG_unevictable,		/* Page is "unevictable"  */
#ifdef CONFIG_MMU
	PG_mlocked,		/* Page is vma mlocked */
#endif
#ifdef CONFIG_ARCH_USES_PG_UNCACHED
	PG_uncached,		/* Page has been mapped as uncached */
#endif
#ifdef CONFIG_MEMORY_FAILURE
	PG_hwpoison,		/* hardware poisoned page. Don't touch */
#endif
#if defined(CONFIG_IDLE_PAGE_TRACKING) && defined(CONFIG_64BIT)
	PG_young,
	PG_idle,
#endif
	__NR_PAGEFLAGS,

	/* Filesystems */
	PG_checked = PG_owner_priv_1,

	/* SwapBacked */
	PG_swapcache = PG_owner_priv_1,	/* Swap page: swp_entry_t in private */

	/* Two page bits are conscripted by FS-Cache to maintain local caching
	 * state.  These bits are set on pages belonging to the netfs's inodes
	 * when those inodes are being locally cached.
	 */
	PG_fscache = PG_private_2,	/* page backed by cache */

	/* XEN */
	/* Pinned in Xen as a read-only pagetable page. */
	PG_pinned = PG_owner_priv_1,
	/* Pinned as part of domain save (see xen_mm_pin_all()). */
	PG_savepinned = PG_dirty,
	/* Has a grant mapping of another (foreign) domain's page. */
	PG_foreign = PG_owner_priv_1,

	/* SLOB */
	PG_slob_free = PG_private,

	/* Compound pages. Stored in first tail page's flags */
	PG_double_map = PG_private_2,

	/* non-lru isolated movable page */
	PG_isolated = PG_reclaim,
};

#ifndef __GENERATING_BOUNDS_H

struct page;	/* forward declaration */

/*
 * page의 compound_head 멤버변수의 0번비트가 1이면 compound 페이지의
 * tail 페이지이다. tail 페이지일 경우는 compound_head 에서 1을
 * 빼서 compound 페이지의 head 페이지 주소를 반환하고
 * 그렇지 않으면 넘어온 매개변수 페이지를 그래도 반환한다.
 */
static inline struct page *compound_head(struct page *page)
{
	unsigned long head = READ_ONCE(page->compound_head);

	if (unlikely(head & 1))
		return (struct page *) (head - 1);
	return page;
}

/* compound_head 의 0번 비트가 1이면 tail 페이지이다. tail 페이지이면 1 아니면 0 반환 */
static __always_inline int PageTail(struct page *page)
{
	return READ_ONCE(page->compound_head) & 1;
}

/* compound head 나 tail 페이지 일 경우 1 반환 아니면 0 반환 */
static __always_inline int PageCompound(struct page *page)
{
	return test_bit(PG_head, &page->flags) || PageTail(page);
}

#define	PAGE_POISON_PATTERN	-1l
static inline int PagePoisoned(const struct page *page)
{
	return page->flags == PAGE_POISON_PATTERN;
}

#ifdef CONFIG_DEBUG_VM
void page_init_poison(struct page *page, size_t size);
#else
static inline void page_init_poison(struct page *page, size_t size)
{
}
#endif

/*
 * 페이지 flags 정책 wrt compound 페이지
 *
 * PF_POISONED_CHECK
 *     이 페이지 구조체가 감염/초기화안됨 을 확인
 *
 * PF_ANY:
 *     페이지 flag가 small, head, 와 tail 페이지에 관련
 *
 * PF_HEAD:
 *     compound 페이지의 경우, 페이지 flag에 관련된 모든 동작이 head 페이지에 적용된다
 *
 * PF_ONLY_HEAD:
 *     compound 페이지의 경우 호출자는 head 페이지서만 작동한다.
 *
 * PF_NO_TAIL:
 *     페이지 flag의 변경은 small 이나 head 페이지 에서만 하여야 한다.
 *     검사는 tail 페이지 에서도 수행 될 수 있다.
 *
 * PF_NO_COMPOUND:
 *     페이지 flag는 compound 페이지와 관련이 없다.
 */
#define PF_POISONED_CHECK(page) ({					\
		VM_BUG_ON_PGFLAGS(PagePoisoned(page), page);		\
		page; })
#define PF_ANY(page, enforce)	PF_POISONED_CHECK(page)
#define PF_HEAD(page, enforce)	PF_POISONED_CHECK(compound_head(page))
#define PF_ONLY_HEAD(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(PageTail(page), page);		\
		PF_POISONED_CHECK(page); })
#define PF_NO_TAIL(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(enforce && PageTail(page), page);	\
		PF_POISONED_CHECK(compound_head(page)); })
#define PF_NO_COMPOUND(page, enforce) ({				\
		VM_BUG_ON_PGFLAGS(enforce && PageCompound(page), page);	\
		PF_POISONED_CHECK(page); })

/*
 * page flags에 대한 함수 정의를 만드는 매크로
 */
#define TESTPAGEFLAG(uname, lname, policy)				\
static __always_inline int Page##uname(struct page *page)		\
	{ return test_bit(PG_##lname, &policy(page, 0)->flags); }

#define SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void SetPage##uname(struct page *page)		\
	{ set_bit(PG_##lname, &policy(page, 1)->flags); }

#define CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void ClearPage##uname(struct page *page)		\
	{ clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define __SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void __SetPage##uname(struct page *page)		\
	{ __set_bit(PG_##lname, &policy(page, 1)->flags); }

#define __CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void __ClearPage##uname(struct page *page)	\
	{ __clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define TESTSETFLAG(uname, lname, policy)				\
static __always_inline int TestSetPage##uname(struct page *page)	\
	{ return test_and_set_bit(PG_##lname, &policy(page, 1)->flags); }

#define TESTCLEARFLAG(uname, lname, policy)				\
static __always_inline int TestClearPage##uname(struct page *page)	\
	{ return test_and_clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define PAGEFLAG(uname, lname, policy)					\
	TESTPAGEFLAG(uname, lname, policy)				\
	SETPAGEFLAG(uname, lname, policy)				\
	CLEARPAGEFLAG(uname, lname, policy)

#define __PAGEFLAG(uname, lname, policy)				\
	TESTPAGEFLAG(uname, lname, policy)				\
	__SETPAGEFLAG(uname, lname, policy)				\
	__CLEARPAGEFLAG(uname, lname, policy)

#define TESTSCFLAG(uname, lname, policy)				\
	TESTSETFLAG(uname, lname, policy)				\
	TESTCLEARFLAG(uname, lname, policy)

#define TESTPAGEFLAG_FALSE(uname)					\
static inline int Page##uname(const struct page *page) { return 0; }

#define SETPAGEFLAG_NOOP(uname)						\
static inline void SetPage##uname(struct page *page) {  }

#define CLEARPAGEFLAG_NOOP(uname)					\
static inline void ClearPage##uname(struct page *page) {  }

#define __CLEARPAGEFLAG_NOOP(uname)					\
static inline void __ClearPage##uname(struct page *page) {  }

#define TESTSETFLAG_FALSE(uname)					\
static inline int TestSetPage##uname(struct page *page) { return 0; }

#define TESTCLEARFLAG_FALSE(uname)					\
static inline int TestClearPage##uname(struct page *page) { return 0; }

#define PAGEFLAG_FALSE(uname) TESTPAGEFLAG_FALSE(uname)			\
	SETPAGEFLAG_NOOP(uname) CLEARPAGEFLAG_NOOP(uname)

#define TESTSCFLAG_FALSE(uname)						\
	TESTSETFLAG_FALSE(uname) TESTCLEARFLAG_FALSE(uname)

__PAGEFLAG(Locked, locked, PF_NO_TAIL)
PAGEFLAG(Waiters, waiters, PF_ONLY_HEAD) __CLEARPAGEFLAG(Waiters, waiters, PF_ONLY_HEAD)
PAGEFLAG(Error, error, PF_NO_COMPOUND) TESTCLEARFLAG(Error, error, PF_NO_COMPOUND)
PAGEFLAG(Referenced, referenced, PF_HEAD)
	TESTCLEARFLAG(Referenced, referenced, PF_HEAD)
	__SETPAGEFLAG(Referenced, referenced, PF_HEAD)
PAGEFLAG(Dirty, dirty, PF_HEAD) TESTSCFLAG(Dirty, dirty, PF_HEAD)
	__CLEARPAGEFLAG(Dirty, dirty, PF_HEAD)
PAGEFLAG(LRU, lru, PF_HEAD) __CLEARPAGEFLAG(LRU, lru, PF_HEAD)
PAGEFLAG(Active, active, PF_HEAD) __CLEARPAGEFLAG(Active, active, PF_HEAD)
	TESTCLEARFLAG(Active, active, PF_HEAD)
PAGEFLAG(Workingset, workingset, PF_HEAD)
	TESTCLEARFLAG(Workingset, workingset, PF_HEAD)
__PAGEFLAG(Slab, slab, PF_NO_TAIL)
__PAGEFLAG(SlobFree, slob_free, PF_NO_TAIL)
PAGEFLAG(Checked, checked, PF_NO_COMPOUND)	   /* Used by some filesystems */

/* Xen */
PAGEFLAG(Pinned, pinned, PF_NO_COMPOUND)
	TESTSCFLAG(Pinned, pinned, PF_NO_COMPOUND)
PAGEFLAG(SavePinned, savepinned, PF_NO_COMPOUND);
PAGEFLAG(Foreign, foreign, PF_NO_COMPOUND);

PAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
	__CLEARPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
	__SETPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
PAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)
	__CLEARPAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)
	__SETPAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)

/*
 * Private page markings that may be used by the filesystem that owns the page
 * for its own purposes.
 * - PG_private and PG_private_2 cause releasepage() and co to be invoked
 */
PAGEFLAG(Private, private, PF_ANY) __SETPAGEFLAG(Private, private, PF_ANY)
	__CLEARPAGEFLAG(Private, private, PF_ANY)
PAGEFLAG(Private2, private_2, PF_ANY) TESTSCFLAG(Private2, private_2, PF_ANY)
PAGEFLAG(OwnerPriv1, owner_priv_1, PF_ANY)
	TESTCLEARFLAG(OwnerPriv1, owner_priv_1, PF_ANY)

/*
 * Only test-and-set exist for PG_writeback.  The unconditional operators are
 * risky: they bypass page accounting.
 */
TESTPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
	TESTSCFLAG(Writeback, writeback, PF_NO_TAIL)
PAGEFLAG(MappedToDisk, mappedtodisk, PF_NO_TAIL)

/* PG_readahead is only used for reads; PG_reclaim is only for writes */
PAGEFLAG(Reclaim, reclaim, PF_NO_TAIL)
	TESTCLEARFLAG(Reclaim, reclaim, PF_NO_TAIL)
PAGEFLAG(Readahead, reclaim, PF_NO_COMPOUND)
	TESTCLEARFLAG(Readahead, reclaim, PF_NO_COMPOUND)

#ifdef CONFIG_HIGHMEM
/*
 * Must use a macro here due to header dependency issues. page_zone() is not
 * available at this point.
 */
#define PageHighMem(__p) is_highmem_idx(page_zonenum(__p))
#else
PAGEFLAG_FALSE(HighMem)
#endif

#ifdef CONFIG_SWAP
static __always_inline int PageSwapCache(struct page *page)
{
#ifdef CONFIG_THP_SWAP
	page = compound_head(page);
#endif
	return PageSwapBacked(page) && test_bit(PG_swapcache, &page->flags);

}
SETPAGEFLAG(SwapCache, swapcache, PF_NO_TAIL)
CLEARPAGEFLAG(SwapCache, swapcache, PF_NO_TAIL)
#else
PAGEFLAG_FALSE(SwapCache)
#endif

PAGEFLAG(Unevictable, unevictable, PF_HEAD)
	__CLEARPAGEFLAG(Unevictable, unevictable, PF_HEAD)
	TESTCLEARFLAG(Unevictable, unevictable, PF_HEAD)

#ifdef CONFIG_MMU
PAGEFLAG(Mlocked, mlocked, PF_NO_TAIL)
	__CLEARPAGEFLAG(Mlocked, mlocked, PF_NO_TAIL)
	TESTSCFLAG(Mlocked, mlocked, PF_NO_TAIL)
#else
PAGEFLAG_FALSE(Mlocked) __CLEARPAGEFLAG_NOOP(Mlocked)
	TESTSCFLAG_FALSE(Mlocked)
#endif

#ifdef CONFIG_ARCH_USES_PG_UNCACHED
PAGEFLAG(Uncached, uncached, PF_NO_COMPOUND)
#else
PAGEFLAG_FALSE(Uncached)
#endif

#ifdef CONFIG_MEMORY_FAILURE
PAGEFLAG(HWPoison, hwpoison, PF_ANY)
TESTSCFLAG(HWPoison, hwpoison, PF_ANY)
#define __PG_HWPOISON (1UL << PG_hwpoison)
extern bool set_hwpoison_free_buddy_page(struct page *page);
#else
PAGEFLAG_FALSE(HWPoison)
static inline bool set_hwpoison_free_buddy_page(struct page *page)
{
	return 0;
}
#define __PG_HWPOISON 0
#endif

#if defined(CONFIG_IDLE_PAGE_TRACKING) && defined(CONFIG_64BIT)
TESTPAGEFLAG(Young, young, PF_ANY)
SETPAGEFLAG(Young, young, PF_ANY)
TESTCLEARFLAG(Young, young, PF_ANY)
PAGEFLAG(Idle, idle, PF_ANY)
#endif

/*
 * On an anonymous page mapped into a user virtual memory area,
 * page->mapping points to its anon_vma, not to a struct address_space;
 * with the PAGE_MAPPING_ANON bit set to distinguish it.  See rmap.h.
 *
 * On an anonymous page in a VM_MERGEABLE area, if CONFIG_KSM is enabled,
 * the PAGE_MAPPING_MOVABLE bit may be set along with the PAGE_MAPPING_ANON
 * bit; and then page->mapping points, not to an anon_vma, but to a private
 * structure which KSM associates with that merged page.  See ksm.h.
 *
 * PAGE_MAPPING_KSM without PAGE_MAPPING_ANON is used for non-lru movable
 * page and then page->mapping points a struct address_space.
 *
 * Please note that, confusingly, "page_mapping" refers to the inode
 * address_space which maps the page from disk; whereas "page_mapped"
 * refers to user virtual address space into which the page is mapped.
 */
#define PAGE_MAPPING_ANON	0x1
#define PAGE_MAPPING_MOVABLE	0x2
#define PAGE_MAPPING_KSM	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)
#define PAGE_MAPPING_FLAGS	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)

static __always_inline int PageMappingFlags(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) != 0;
}

static __always_inline int PageAnon(struct page *page)
{
	page = compound_head(page);
	return ((unsigned long)page->mapping & PAGE_MAPPING_ANON) != 0;
}

static __always_inline int __PageMovable(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) ==
				PAGE_MAPPING_MOVABLE;
}

#ifdef CONFIG_KSM
/*
 * A KSM page is one of those write-protected "shared pages" or "merged pages"
 * which KSM maps into multiple mms, wherever identical anonymous page content
 * is found in VM_MERGEABLE vmas.  It's a PageAnon page, pointing not to any
 * anon_vma, but to that page's node of the stable tree.
 */
static __always_inline int PageKsm(struct page *page)
{
	page = compound_head(page);
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) ==
				PAGE_MAPPING_KSM;
}
#else
TESTPAGEFLAG_FALSE(Ksm)
#endif

u64 stable_page_flags(struct page *page);

static inline int PageUptodate(struct page *page)
{
	int ret;
	page = compound_head(page);
	ret = test_bit(PG_uptodate, &(page)->flags);
	/*
	 * Must ensure that the data we read out of the page is loaded
	 * _after_ we've loaded page->flags to check for PageUptodate.
	 * We can skip the barrier if the page is not uptodate, because
	 * we wouldn't be reading anything from it.
	 *
	 * See SetPageUptodate() for the other side of the story.
	 */
	if (ret)
		smp_rmb();

	return ret;
}

static __always_inline void __SetPageUptodate(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	smp_wmb();
	__set_bit(PG_uptodate, &page->flags);
}

static __always_inline void SetPageUptodate(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	/*
	 * Memory barrier must be issued before setting the PG_uptodate bit,
	 * so that all previous stores issued in order to bring the page
	 * uptodate are actually visible before PageUptodate becomes true.
	 */
	smp_wmb();
	set_bit(PG_uptodate, &page->flags);
}

CLEARPAGEFLAG(Uptodate, uptodate, PF_NO_TAIL)

int test_clear_page_writeback(struct page *page);
int __test_set_page_writeback(struct page *page, bool keep_write);

#define test_set_page_writeback(page)			\
	__test_set_page_writeback(page, false)
#define test_set_page_writeback_keepwrite(page)	\
	__test_set_page_writeback(page, true)

static inline void set_page_writeback(struct page *page)
{
	test_set_page_writeback(page);
}

static inline void set_page_writeback_keepwrite(struct page *page)
{
	test_set_page_writeback_keepwrite(page);
}

__PAGEFLAG(Head, head, PF_ANY) CLEARPAGEFLAG(Head, head, PF_ANY)

static __always_inline void set_compound_head(struct page *page, struct page *head)
{
	WRITE_ONCE(page->compound_head, (unsigned long)head + 1);
}

static __always_inline void clear_compound_head(struct page *page)
{
	WRITE_ONCE(page->compound_head, 0);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline void ClearPageCompound(struct page *page)
{
	BUG_ON(!PageHead(page));
	ClearPageHead(page);
}
#endif

#define PG_head_mask ((1UL << PG_head))

#ifdef CONFIG_HUGETLB_PAGE
int PageHuge(struct page *page);
int PageHeadHuge(struct page *page);
bool page_huge_active(struct page *page);
#else
TESTPAGEFLAG_FALSE(Huge)
TESTPAGEFLAG_FALSE(HeadHuge)

static inline bool page_huge_active(struct page *page)
{
	return 0;
}
#endif


#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * PageHuge() only returns true for hugetlbfs pages, but not for
 * normal or transparent huge pages.
 *
 * PageTransHuge() returns true for both transparent huge and
 * hugetlbfs pages, but not normal pages. PageTransHuge() can only be
 * called only in the core VM paths where hugetlbfs pages can't exist.
 */
static inline int PageTransHuge(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	return PageHead(page);
}

/*
 * PageTransCompound returns true for both transparent huge pages
 * and hugetlbfs pages, so it should only be called when it's known
 * that hugetlbfs pages aren't involved.
 */
static inline int PageTransCompound(struct page *page)
{
	return PageCompound(page);
}

/*
 * PageTransCompoundMap is the same as PageTransCompound, but it also
 * guarantees the primary MMU has the entire compound page mapped
 * through pmd_trans_huge, which in turn guarantees the secondary MMUs
 * can also map the entire compound page. This allows the secondary
 * MMUs to call get_user_pages() only once for each compound page and
 * to immediately map the entire compound page with a single secondary
 * MMU fault. If there will be a pmd split later, the secondary MMUs
 * will get an update through the MMU notifier invalidation through
 * split_huge_pmd().
 *
 * Unlike PageTransCompound, this is safe to be called only while
 * split_huge_pmd() cannot run from under us, like if protected by the
 * MMU notifier, otherwise it may result in page->_mapcount < 0 false
 * positives.
 */
static inline int PageTransCompoundMap(struct page *page)
{
	return PageTransCompound(page) && atomic_read(&page->_mapcount) < 0;
}

/*
 * PageTransTail returns true for both transparent huge pages
 * and hugetlbfs pages, so it should only be called when it's known
 * that hugetlbfs pages aren't involved.
 */
static inline int PageTransTail(struct page *page)
{
	return PageTail(page);
}

/*
 * PageDoubleMap indicates that the compound page is mapped with PTEs as well
 * as PMDs.
 *
 * This is required for optimization of rmap operations for THP: we can postpone
 * per small page mapcount accounting (and its overhead from atomic operations)
 * until the first PMD split.
 *
 * For the page PageDoubleMap means ->_mapcount in all sub-pages is offset up
 * by one. This reference will go away with last compound_mapcount.
 *
 * See also __split_huge_pmd_locked() and page_remove_anon_compound_rmap().
 */
static inline int PageDoubleMap(struct page *page)
{
	return PageHead(page) && test_bit(PG_double_map, &page[1].flags);
}

static inline void SetPageDoubleMap(struct page *page)
{
	VM_BUG_ON_PAGE(!PageHead(page), page);
	set_bit(PG_double_map, &page[1].flags);
}

static inline void ClearPageDoubleMap(struct page *page)
{
	VM_BUG_ON_PAGE(!PageHead(page), page);
	clear_bit(PG_double_map, &page[1].flags);
}
static inline int TestSetPageDoubleMap(struct page *page)
{
	VM_BUG_ON_PAGE(!PageHead(page), page);
	return test_and_set_bit(PG_double_map, &page[1].flags);
}

static inline int TestClearPageDoubleMap(struct page *page)
{
	VM_BUG_ON_PAGE(!PageHead(page), page);
	return test_and_clear_bit(PG_double_map, &page[1].flags);
}

#else
TESTPAGEFLAG_FALSE(TransHuge)
TESTPAGEFLAG_FALSE(TransCompound)
TESTPAGEFLAG_FALSE(TransCompoundMap)
TESTPAGEFLAG_FALSE(TransTail)
PAGEFLAG_FALSE(DoubleMap)
	TESTSETFLAG_FALSE(DoubleMap)
	TESTCLEARFLAG_FALSE(DoubleMap)
#endif

/*
 * For pages that are never mapped to userspace (and aren't PageSlab),
 * page_type may be used.  Because it is initialised to -1, we invert the
 * sense of the bit, so __SetPageFoo *clears* the bit used for PageFoo, and
 * __ClearPageFoo *sets* the bit used for PageFoo.  We reserve a few high and
 * low bits so that an underflow or overflow of page_mapcount() won't be
 * mistaken for a page type value.
 */

#define PAGE_TYPE_BASE	0xf0000000
/* Reserve		0x0000007f to catch underflows of page_mapcount */
#define PAGE_MAPCOUNT_RESERVE	-128
#define PG_buddy	0x00000080
#define PG_offline	0x00000100
#define PG_kmemcg	0x00000200
#define PG_table	0x00000400

#define PageType(page, flag)						\
	((page->page_type & (PAGE_TYPE_BASE | flag)) == PAGE_TYPE_BASE)

static inline int page_has_type(struct page *page)
{
	return (int)page->page_type < PAGE_MAPCOUNT_RESERVE;
}

#define PAGE_TYPE_OPS(uname, lname)					\
static __always_inline int Page##uname(struct page *page)		\
{									\
	return PageType(page, PG_##lname);				\
}									\
static __always_inline void __SetPage##uname(struct page *page)		\
{									\
	VM_BUG_ON_PAGE(!PageType(page, 0), page);			\
	page->page_type &= ~PG_##lname;					\
}									\
static __always_inline void __ClearPage##uname(struct page *page)	\
{									\
	VM_BUG_ON_PAGE(!Page##uname(page), page);			\
	page->page_type |= PG_##lname;					\
}

/*
 * PageBuddy() indicates that the page is free and in the buddy system
 * (see mm/page_alloc.c).
 */
PAGE_TYPE_OPS(Buddy, buddy)

/*
 * PageOffline() indicates that the page is logically offline although the
 * containing section is online. (e.g. inflated in a balloon driver or
 * not onlined when onlining the section).
 * The content of these pages is effectively stale. Such pages should not
 * be touched (read/write/dump/save) except by their owner.
 */
PAGE_TYPE_OPS(Offline, offline)

/*
 * If kmemcg is enabled, the buddy allocator will set PageKmemcg() on
 * pages allocated with __GFP_ACCOUNT. It gets cleared on page free.
 */
PAGE_TYPE_OPS(Kmemcg, kmemcg)

/*
 * Marks pages in use as page tables.
 */
PAGE_TYPE_OPS(Table, table)

extern bool is_free_buddy_page(struct page *page);

__PAGEFLAG(Isolated, isolated, PF_ANY);

/*
 * If network-based swap is enabled, sl*b must keep track of whether pages
 * were allocated from pfmemalloc reserves.
 */
static inline int PageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	return PageActive(page);
}

static inline void SetPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	SetPageActive(page);
}

static inline void __ClearPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	__ClearPageActive(page);
}

static inline void ClearPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	ClearPageActive(page);
}

#ifdef CONFIG_MMU
#define __PG_MLOCKED		(1UL << PG_mlocked)
#else
#define __PG_MLOCKED		0
#endif

/*
 * Flags checked when a page is freed.  Pages being freed should not have
 * these flags set.  It they are, there is a problem.
 */
#define PAGE_FLAGS_CHECK_AT_FREE				\
	(1UL << PG_lru		| 1UL << PG_locked	|	\
	 1UL << PG_private	| 1UL << PG_private_2	|	\
	 1UL << PG_writeback	| 1UL << PG_reserved	|	\
	 1UL << PG_slab		| 1UL << PG_active 	|	\
	 1UL << PG_unevictable	| __PG_MLOCKED)

/*
 * Flags checked when a page is prepped for return by the page allocator.
 * Pages being prepped should not have these flags set.  It they are set,
 * there has been a kernel bug or struct page corruption.
 *
 * __PG_HWPOISON is exceptional because it needs to be kept beyond page's
 * alloc-free cycle to prevent from reusing the page.
 */
#define PAGE_FLAGS_CHECK_AT_PREP	\
	(((1UL << NR_PAGEFLAGS) - 1) & ~__PG_HWPOISON)

#define PAGE_FLAGS_PRIVATE				\
	(1UL << PG_private | 1UL << PG_private_2)
/**
 * page_has_private - Determine if page has private stuff
 * @page: The page to be checked
 *
 * Determine if a page has private stuff, indicating that release routines
 * should be invoked upon it.
 */
static inline int page_has_private(struct page *page)
{
	return !!(page->flags & PAGE_FLAGS_PRIVATE);
}

#undef PF_ANY
#undef PF_HEAD
#undef PF_ONLY_HEAD
#undef PF_NO_TAIL
#undef PF_NO_COMPOUND
#endif /* !__GENERATING_BOUNDS_H */

#endif	/* PAGE_FLAGS_H */
