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
/* compound tail 페이지 이면 빌드 에러. 아니면 페이지 반환하되 하드웨어 문제 있으면 빌드 버그 */
#define PF_ONLY_HEAD(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(PageTail(page), page);		\
		PF_POISONED_CHECK(page); })
/* enforce 가 1이고, compound tail 페이지 이면 빌드 에러 아니면 enforce 0일때 수행 */
/* enforce 가 0 이면 compound head 나 페이지 그대로 반환하되 하드웨어 문제 있으면 빌드 버그 */
#define PF_NO_TAIL(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(enforce && PageTail(page), page);	\
		PF_POISONED_CHECK(compound_head(page)); })
/* enforce 가 1이고, compound 페이지 이면 빌드에러 아니면 enforce 0 일때 수행*/
/* enforce 가 0이면 페이지 그대로 반환하되 하드웨어 문제 있으면 빌드 버그 */
#define PF_NO_COMPOUND(page, enforce) ({				\
		VM_BUG_ON_PGFLAGS(enforce && PageCompound(page), page);	\
		PF_POISONED_CHECK(page); })

/*
 * page flags에 대한 함수 정의를 만드는 매크로
 */
/*
 * PageUname 함수를 만드는 매크로. 페이지의 flags 멤버 변수에 PG_lname 비트가
 * 설정되었으면 1을 반환. 세번째 매개변수 policy는 위 PF_* 매크로가 반환하는 페이지이다.
 * enforce는 0로 설정된다.
 * ex. PGLRU(page) page->flags의 PG_lru(4번)비트가 설정되었으면 1을 반환
 */
#define TESTPAGEFLAG(uname, lname, policy)				\
static __always_inline int Page##uname(struct page *page)		\
	{ return test_bit(PG_##lname, &policy(page, 0)->flags); }

/*
 * SetPageUname 함수를 만드는 매크로. 페이지의 flags멤버 변수에 PG_lname 비트를
 * 설정한다. 세번째 매개변수 policy는 위 PF_* 매크로가 반환하는 페이지이다.
 * enforce는 1로 설정된다
 * ex. SetPageLRU(page) page->flags의 PG_lru(4번)비트를 설정.
 */
#define SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void SetPage##uname(struct page *page)		\
	{ set_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * ClearPageUname 함수를 만드는 매크로. 페이지의 flags멤버 변수에 PG_lname 비트를
 * 지운다. 세번째 매개변수 policy는 위 PF_* 매크로가 반환하는 페이지이다.
 * enforce는 1로 설정된다
 * ex. ClearPageLRU(page) page->flags의 PG_lru(4번)비트를 지움.
 */
#define CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void ClearPage##uname(struct page *page)		\
	{ clear_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * __SetPageUname 함수를 만드는 매크로. SetPageUname 과 같지만 원자적이 아니다.
 */
#define __SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void __SetPage##uname(struct page *page)		\
	{ __set_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * __ClearPageUname 함수를 만드는 매크로. ClearPageUname 과 같지만 원자적이 아니다.
 */
#define __CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void __ClearPage##uname(struct page *page)	\
	{ __clear_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * TestSetPageUname 함수를 만드는 매크로. PG_lname에 해당하는 bit를 설정하고
 * 해당 비트의 원래 값을 반환한다. enforce는 1로 설정
 * ex. TestSetPageLRU(page) page->flags의 PG_lru(4번)비트를 설정하고
 * 원래 PG_lru(4번)비트에 설정된 값 반환.
 */
#define TESTSETFLAG(uname, lname, policy)				\
static __always_inline int TestSetPage##uname(struct page *page)	\
	{ return test_and_set_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * TestClearPageUname 함수를 만드는 매크로. PG_lname에 해당하는 bit를 지우고
 * 해당 비트의 원래 값을 반환한다. enforce는 1로 설정
 * ex. TestCleartPageLRU(page) page->flags의 PG_lru(4번)비트를 지우고
 * 원래 PG_lru(4번)비트에 설정된 값 반환.
 */
#define TESTCLEARFLAG(uname, lname, policy)				\
static __always_inline int TestClearPage##uname(struct page *page)	\
	{ return test_and_clear_bit(PG_##lname, &policy(page, 1)->flags); }

/*
 * PAGEFLAG 매크로를 사용하여 PageUname, SetPageUname, ClearPageUname 세개
 * 함수를 만든다. __PAGEFLAG 매크로는 같은 종류의 non-atomic 함수를 만든다.
 * TESTSCFLAG 는 이전 값을 반환하므로 테스트 함수는 뺀 2가지만 만든다.
 */
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

/* 무조건 0을 반환하는 PageUname 함수와 빈 Set, Clear 함수를 만든다 */
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
 * 자체 목적으로 페이지를 소유하는 파일 시스템에서 사용할 수있는 Private 페이지 표시
 * PG_private 와 PG_private_2로 인해 releasepage()및 co 가 호출된다.
 */
PAGEFLAG(Private, private, PF_ANY) __SETPAGEFLAG(Private, private, PF_ANY)
	__CLEARPAGEFLAG(Private, private, PF_ANY)
PAGEFLAG(Private2, private_2, PF_ANY) TESTSCFLAG(Private2, private_2, PF_ANY)
PAGEFLAG(OwnerPriv1, owner_priv_1, PF_ANY)
	TESTCLEARFLAG(OwnerPriv1, owner_priv_1, PF_ANY)

/*
 * PG_writeback에 대한 test-and-set만 존재한다. 무조적적인 명령들은
 * 위험하다. : 페이지 감사를 건너뛴다.
 */
TESTPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
	TESTSCFLAG(Writeback, writeback, PF_NO_TAIL)
PAGEFLAG(MappedToDisk, mappedtodisk, PF_NO_TAIL)

/* PG_readahead 는 읽기에만 사용된다; PG_reclaim은 쓰기전용이다. */
PAGEFLAG(Reclaim, reclaim, PF_NO_TAIL)
	TESTCLEARFLAG(Reclaim, reclaim, PF_NO_TAIL)
PAGEFLAG(Readahead, reclaim, PF_NO_COMPOUND)
	TESTCLEARFLAG(Readahead, reclaim, PF_NO_COMPOUND)

#ifdef CONFIG_HIGHMEM
/*
 * header 종속성 문제 때문에 여기 매크로를 사용해야 한다. page_zone은
 * 여기에 가용하지 않다.
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
 * 사용자 가상 메모리 영역에 매핑 된 익명 페이지에서 page->mapping은 그것을 구분하기 위한
 * PAGE_MAPPING_ANON 비트 설정과 함께 address_space 구조체가 아닌 anon_vma를 가리킨다.
 * rmap.h 참조
 *
 * VM_MERGEABLE 영역의 익명 페이지는 만약 CONFIG_KSM이 활성화 되었다면
 * PAGE_MAPPING_MOVABLE 비트가 PAGE_MAPPING_ANON 비트와 함께 설정 될 수 있다.
 * page->mapping 은 anon_vma 가 아니라 KSM이 병합된 페이지와 함께 관련한
 * private 구조체를 가리킨다. ksm.h 참조
 *
 * PAGE_MAPPING_ANON 이 없는 PAGE_MAPPING_KSM은 non-lru movable 페이지를 위해
 * 사용되고 page->mapping 은 address_space 구조체를 가리킨다.
 *
 * 디스크로부터 페이지를 매핑하여 inode address_space에연관된 "page_mapping"과
 * 페이지가 매핑되는 사용자 가상 주소 영역과 연관된 "page mapped"가
 * 혼동하지 않게 주의하세요
 */
#define PAGE_MAPPING_ANON	0x1
#define PAGE_MAPPING_MOVABLE	0x2
#define PAGE_MAPPING_KSM	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)
#define PAGE_MAPPING_FLAGS	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)

/* PAGE_MAPPING_ANON나 PAGE_MAPPING_MOVABLE이 설정 되었으면 1 반환 */
static __always_inline int PageMappingFlags(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) != 0;
}

/* PAGE_MAPPING_ANON 가 설정되었으면 1 반환 */
static __always_inline int PageAnon(struct page *page)
{
	page = compound_head(page);
	return ((unsigned long)page->mapping & PAGE_MAPPING_ANON) != 0;
}

/*
 * PAGE_MAPPING_ANON는 설정되지 않고 PAGE_MAPPING_MOVABLE만
 * 설정되었을시(non-lru movable 페이지) 1 반환
 */
static __always_inline int __PageMovable(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) ==
				PAGE_MAPPING_MOVABLE;
}

#ifdef CONFIG_KSM
/*
 * KSM 페이지는 KSM이 동일한 익명 페이지 항목이 VM_MERGEABLE vma에 발견될 때 마다
 * 다수의 mms에 매핑하는 쓰기 보호된 공유 페이지들이나 병합페이지들의 하나이다.
 * anon_vma 가 아니라 안정 tree의 해당 페이지 노드를 가리키는 PageAnon 페이지이다.
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
	 * 페이지 에서 읽은 데이터는 PageUptodate를 검사하기 위해 page->flags를
	 * 로드한 후에 로드되었다는 것을 보장해야 한다. 만약 페이지가 uptodate가
	 * 아니면 아무것도 읽고 있지 않을것이므로 배리어를 건너뛸 수 있다.
	 *
	 * 다른부분을 확인 하려면 SetPageUptodate 참조
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
	 * 메모리 배리어는 PG_uptodate 비트가 설정되기 전에 발행되어야 한다.
	 * 그러면 PageUptodate가 true가 되기 전에 페이지를 최신으로 만들기 위해
	 * 발행된 모든 이전 저장은 실제로 표시된다.
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
 * PageHuge 는 normal 이나 transparent huge pages가 아닌 hugetlbfs 페이지에
 * true를 반환한다.
 *
 * PageTransHuge는 transparent huge와 hugetlbfs 페이지 모두 true를 반환하고
 * normal 페이지는 아니다. PageTransHuge는 hugetlbfs 페이지가 존재할 수 없는
 * core VM 경로에서만 호출 할 수 있다.
 */
static inline int PageTransHuge(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	return PageHead(page);
}

/*
 * PagesTransCompound 는 transparent huge 와 hugetlbfs 페이지 모두에
 * true 를 반환한다. hugetlbfs 페이지가 관련되지 않은것으로 알려진 경우에만
 * 호출해야 한다.
 */
static inline int PageTransCompound(struct page *page)
{
	return PageCompound(page);
}

/*
 * PageTransCompoundMap은 PageTransCompound와 같지만 기본 MMU가
 * pmd_trans_huge를 통해 매핑된 전체 compound 페이지가 있음을 보장한다.
 * 따라서 보조 MMU도 전체 compound 페이지를 매핑할 수 있음을 보장한다.
 * 이것은 보조 MMU가 각각의 compound 페이지에 get_user_pages를
 * 한번만 호출하고, 단일 보조 MMU 실패와 함께 전체 compound 페이지가
 * 즉시 매핑되도록 한다. pmd가 후에 분리 되더라도 보조 MMU는
 * split_hug_pmd를 통한 MMU notifies invalidation을 통해 업데이트 된다.
 *
 * PageTransCompound와 달리 MMU notifiers에 의해 보호되는 것처럼
 * split_huge_pmd가 실행될 수 없을 때 호출 하는 것이 안전하다. 그렇지 않으면
 * page->mapcount < 0 오탐이 발생할 수 있다.
 */
static inline int PageTransCompoundMap(struct page *page)
{
	return PageTransCompound(page) && atomic_read(&page->_mapcount) < 0;
}

/*
 * PagesTransTail은 transparent huge와 hugetlbfs 페이지 모두 true를 반환한다.
 * 따라서 hugetlbfs 페이지와 관련없음으로 알려진 경우에만 호출해야 한다
 */
static inline int PageTransTail(struct page *page)
{
	return PageTail(page);
}

/*
 * PageDoubleMap은 compound 페이지가 PTE와 PMD로 매핑되었을 음을 나타낸다.
 *
 * 이는 THP에 rmap작업의 최적화를 위해 필요하다. 첫번째 PMD 분활전까지 작은 페이지당
 * mapcount 세기를 연기할 수 있다.(및 atomic 명령의 부작용)
 *
 * 페이지에서 PageDoublemap은 모든 하위 페이지의 _mapcount가 1 만큼 오프셋됨을
 * 의미한다. . 이 참조는 마지막 compound_mapcount와 함께 사라진다.
 *
 * __split_huge_pmd_locked() and page_remove_anon_compound_rmap() 참조
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
 * 유저영역에 매핑된적 없는 페이지(및 PageSlab 이 아닌), page_type을 사용할 수 있다.
 * -1로 초기화 되므로 비트를 반전한다. 그래서 __SetPageFoo는 PageFoo 가 사용하는
 * 비트를 지우고 __ClearPageFoo는 PageFoo가 사용하는 비트를 설정한다. 몇개의 상위
 * 하위 비트를 예약함으로서 page_mapcount의 underflow나 overflow 가
 * 페이지 type 값으로 오인되지 않도록 한다.
 */
#define PAGE_TYPE_BASE	0xf0000000
/* Reserve		0x0000007f to catch underflows of page_mapcount */
#define PAGE_MAPCOUNT_RESERVE	-128 /* 0xffffff80 */
#define PG_buddy	0x00000080   /* 0xffffff7f, -129 */
#define PG_offline	0x00000100   /* 0xfffffEff, -257 */
#define PG_kmemcg	0x00000200   /* 0xfffffdff, -513 */
#define PG_table	0x00000400   /* 0xfffffbff, -1025 */

/* page가 flag 타입이면 true. */
#define PageType(page, flag)						\
	((page->page_type & (PAGE_TYPE_BASE | flag)) == PAGE_TYPE_BASE)

/*
 * 위치 카운터가 커질수록 점점 큰 양수를 지우게 되므로 숫자는 점점 작아진다
 * 따라서 PAGE_MAPCOUNT_RESERVE(-128)보다 작은 수(위에서 -129, -257등)의 경우
 * type을 가지고 있으므로 참을 반환
 */
static inline int page_has_type(struct page *page)
{
	return (int)page->page_type < PAGE_MAPCOUNT_RESERVE;
}

/*
 * PAGE_TYPE_OPS 매크로를 사용하여 PageUname, __SetPageUname, __ClearPageUname
 * 함수를 만든다.
 */
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
 * PageBuddy 함수는 페이지가 비어있고 버디시스템에 있다는 걸 나타낸다.
 * mm/page_alloc.c 참조
 * 코드상으로는 PG_buddy 비트가 0인지를 리턴한다
 */
/* PageBuddy, __SetPageBuddy, __ClearPageBuddy 함수를 만든다. */
PAGE_TYPE_OPS(Buddy, buddy)

/*
 * PageOffline함수는 포함하는 섹션이 online이지만 페이지가 논리적으로 오프라인 임을
 * 나타낸다.(예. balloon driver에서 부풀려 졌거나 섹션을 온라인화할때 온라인 상태가 아님)
 * 이 페이들의 내용은 사실상 오래되었다. 이러한 페이지들은 소유자 외에는
 * 접근되지 않아야 한다.(읽기/쓰기/dump/저장)
 */
/* PageOffline, __SetPageOffline, __ClearPageOffline 함수를 만든다. */
PAGE_TYPE_OPS(Offline, offline)

/*
 * kemcg가 활성화 되었다면 버디 할당자는  __GFP_ACCOUNT와 함께 할당된 페이지들에
 * PageKmemcg를 설정할 것이다. 페이지 free에서 지워진다.
 */
/* PageKmemcg, __SetPageKmemcg, __ClearPageKmemcg 함수를 만든다. */
PAGE_TYPE_OPS(Kmemcg, kmemcg)

/*
 * 사용중인 페이지를 페이지 테이블로 표시
 */
/* PageTable, __SetPageTable, __ClearPageTable 함수를 만든다. */
PAGE_TYPE_OPS(Table, table)

extern bool is_free_buddy_page(struct page *page);

__PAGEFLAG(Isolated, isolated, PF_ANY);

/*
 * 네트워크기반 스왑이 활성화 되었다면 sl*b는 pfmemalloc 예약에서 페이지가 할당
 * 되었는지 여부를 추적해야 한다.
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
 * 페이지가 해제될 때 확인되는 플래그들. 해제되는 페이지들은 이 플래그들이 설정되어서는
 * 안된다. 만약 그렇다면 문제가 있다.
 */
 #define PAGE_FLAGS_CHECK_AT_FREE				\
	(1UL << PG_lru		| 1UL << PG_locked	|	\
	 1UL << PG_private	| 1UL << PG_private_2	|	\
	 1UL << PG_writeback	| 1UL << PG_reserved	|	\
	 1UL << PG_slab		| 1UL << PG_active 	|	\
	 1UL << PG_unevictable	| __PG_MLOCKED)

/*
 * 페이지 할당자가 페이지 반환을 준비할 때 확인하는 플래그들
 * 준비된 페이지들은 이 플래그 설정이 있어서는 안된다. 설정되었다면 커널 버그이거나
 * 구조체 페이지 손상이다.
 *
 * __PG_HWPOISON 은 페이지 재사용을 막기위해 페이지의 alloc-free 사이클 이상으로
 * 유지되어야 하므로 예외다.
 */
#define PAGE_FLAGS_CHECK_AT_PREP	\
	(((1UL << NR_PAGEFLAGS) - 1) & ~__PG_HWPOISON)

#define PAGE_FLAGS_PRIVATE				\
	(1UL << PG_private | 1UL << PG_private_2)

/*
 * page_has_private - 페이지가 private인지 확인
 * @page: 검사 할 페이지
 *
 * 페이지가 private 항목이 있는지 확인하여, 해제 루틴이 수행되어야 함을
 * 나타냄
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
