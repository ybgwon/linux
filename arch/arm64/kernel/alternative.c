/*
 * alternative runtime patching
 * inspired by the x86 version
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "alternatives: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <asm/cacheflush.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/insn.h>
#include <asm/sections.h>
#include <linux/stop_machine.h>

#define __ALT_PTR(a,f)		((void *)&(a)->f + (a)->f)
#define ALT_ORIG_PTR(a)		__ALT_PTR(a, orig_offset)
#define ALT_REPL_PTR(a)		__ALT_PTR(a, alt_offset)

static int all_alternatives_applied;

static DECLARE_BITMAP(applied_alternatives, ARM64_NCAPS);

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

bool alternative_is_applied(u16 cpufeature)
{
	if (WARN_ON(cpufeature >= ARM64_NCAPS))
		return false;

	return test_bit(cpufeature, applied_alternatives);
}

/*
 * Check if the target PC is within an alternative block.
 */
static bool branch_insn_requires_update(struct alt_instr *alt, unsigned long pc)
{
	unsigned long replptr;

	if (kernel_text_address(pc))
		return true;

	replptr = (unsigned long)ALT_REPL_PTR(alt);
	if (pc >= replptr && pc <= (replptr + alt->alt_len))
		return false;

	/*
	 * Branching into *another* alternate sequence is doomed, and
	 * we're not even trying to fix it up.
	 */
	BUG();
}

#define align_down(x, a)	((unsigned long)(x) & ~(((unsigned long)(a)) - 1))

static u32 get_alt_insn(struct alt_instr *alt, __le32 *insnptr, __le32 *altinsnptr)
{
	u32 insn;

	insn = le32_to_cpu(*altinsnptr);

	if (aarch64_insn_is_branch_imm(insn)) {
		s32 offset = aarch64_get_branch_offset(insn);
		unsigned long target;

		target = (unsigned long)altinsnptr + offset;

		/*
		 * If we're branching inside the alternate sequence,
		 * do not rewrite the instruction, as it is already
		 * correct. Otherwise, generate the new instruction.
		 */
		if (branch_insn_requires_update(alt, target)) {
			offset = target - (unsigned long)insnptr;
			insn = aarch64_set_branch_offset(insn, offset);
		}
	} else if (aarch64_insn_is_adrp(insn)) {
		s32 orig_offset, new_offset;
		unsigned long target;

		/*
		 * If we're replacing an adrp instruction, which uses PC-relative
		 * immediate addressing, adjust the offset to reflect the new
		 * PC. adrp operates on 4K aligned addresses.
		 */
		orig_offset  = aarch64_insn_adrp_get_offset(insn);
		target = align_down(altinsnptr, SZ_4K) + orig_offset;
		new_offset = target - align_down(insnptr, SZ_4K);
		insn = aarch64_insn_adrp_set_offset(insn, new_offset);
	} else if (aarch64_insn_uses_literal(insn)) {
		/*
		 * Disallow patching unhandled instructions using PC relative
		 * literal addresses
		 */
		BUG();
	}

	return insn;
}

static void patch_alternative(struct alt_instr *alt,
			      __le32 *origptr, __le32 *updptr, int nr_inst)
{
	__le32 *replptr;
	int i;

	replptr = ALT_REPL_PTR(alt);
	for (i = 0; i < nr_inst; i++) {
		u32 insn;

		insn = get_alt_insn(alt, origptr + i, replptr + i);
		updptr[i] = cpu_to_le32(insn);
	}
}

/*
 * We provide our own, private D-cache cleaning function so that we don't
 * accidentally call into the cache.S code, which is patched by us at
 * runtime.
 */
static void clean_dcache_range_nopatch(u64 start, u64 end)
{
	u64 cur, d_size, ctr_el0;

	ctr_el0 = read_sanitised_ftr_reg(SYS_CTR_EL0);
	d_size = 4 << cpuid_feature_extract_unsigned_field(ctr_el0,
							   CTR_DMINLINE_SHIFT);
	cur = start & ~(d_size - 1);
	do {
		/*
		 * We must clean+invalidate to the PoC in order to avoid
		 * Cortex-A53 errata 826319, 827319, 824069 and 819472
		 * (this corresponds to ARM64_WORKAROUND_CLEAN_CACHE)
		 */
		asm volatile("dc civac, %0" : : "r" (cur) : "memory");
	} while (cur += d_size, cur < end);
}

static void __apply_alternatives(void *alt_region,  bool is_module,
				 unsigned long *feature_mask)
{
	struct alt_instr *alt;
	struct alt_region *region = alt_region;
	__le32 *origptr, *updptr;
	alternative_cb_t alt_cb;

	for (alt = region->begin; alt < region->end; alt++) {
		int nr_inst;

		// feature_mask 비트맵에 설정되지 않은 cpufeature는 건너뛴다.
		if (!test_bit(alt->cpufeature, feature_mask))
			continue;

		/* ARM64_CB_PATCH 를 무조건 패치로 사용 */
		if (alt->cpufeature < ARM64_CB_PATCH &&
		    !cpus_have_cap(alt->cpufeature))
			continue;

		if (alt->cpufeature == ARM64_CB_PATCH)
			BUG_ON(alt->alt_len != 0);
		else
			BUG_ON(alt->alt_len != alt->orig_len);

		pr_info_once("patching kernel code\n");

		// orig 명령이 있는 주소
		origptr = ALT_ORIG_PTR(alt);
		// 보안을 위해 origptr 을 1:1 영역에 맵한 주소
		updptr = is_module ? origptr : lm_alias(origptr);
		nr_inst = alt->orig_len / AARCH64_INSN_SIZE;

		if (alt->cpufeature < ARM64_CB_PATCH)
			alt_cb = patch_alternative;
		else
			alt_cb  = ALT_REPL_PTR(alt);

		// ALTENATIVE 매크로의 명령을 대체명려으로 교체한다.
		alt_cb(alt, origptr, updptr, nr_inst);

		if (!is_module) {
			clean_dcache_range_nopatch((u64)origptr,
						   (u64)(origptr + nr_inst));
		}
	}

	/*
	 * core 모듈 코드는 flush_module_icache() 에서 cache 관리를 처리한다.
	 */
	if (!is_module) {
		dsb(ish);
		__flush_icache_all();
		isb();

		/* feature mask 에서 ARM64_CB 비트 무시 */
		// applied_alternatives 비트맵에 적용된 기능 표시
		bitmap_or(applied_alternatives, applied_alternatives,
			  feature_mask, ARM64_NCAPS);
		bitmap_and(applied_alternatives, applied_alternatives,
			   cpu_hwcaps, ARM64_NCAPS);
	}
}

/*
 * We might be patching the stop_machine state machine, so implement a
 * really simple polling protocol here.
 */
static int __apply_alternatives_multi_stop(void *unused)
{
	struct alt_region region = {
		.begin	= (struct alt_instr *)__alt_instructions,
		.end	= (struct alt_instr *)__alt_instructions_end,
	};

	/* We always have a CPU 0 at this point (__init) */
	if (smp_processor_id()) {
		while (!READ_ONCE(all_alternatives_applied))
			cpu_relax();
		isb();
	} else {
		DECLARE_BITMAP(remaining_capabilities, ARM64_NPATCHABLE);

		bitmap_complement(remaining_capabilities, boot_capabilities,
				  ARM64_NPATCHABLE);

		BUG_ON(all_alternatives_applied);
		__apply_alternatives(&region, false, remaining_capabilities);
		/* Barriers provided by the cache flushing */
		WRITE_ONCE(all_alternatives_applied, 1);
	}

	return 0;
}

void __init apply_alternatives_all(void)
{
	/* better not try code patching on a live SMP system */
	stop_machine(__apply_alternatives_multi_stop, NULL, cpu_online_mask);
}

/*
 * 이 함수는 부트 프로세스의 매우 초반에 불려진다.(부트 CPU에서 기능 감지를 실행한 직후).
 * 여기에서 다른 CPU에 대해 걱정할 필요가 없다.
 */
void __init apply_boot_alternatives(void)
{
	// __alt_instructions 섹션은 ALTERNATIVE 매크로에 의해
	//  구조체 멤버들이 push 되어 만들어진다.
	struct alt_region region = {
		.begin	= (struct alt_instr *)__alt_instructions,
		.end	= (struct alt_instr *)__alt_instructions_end,
	};

	/* non-boot cpu에서 호출되엇다면 문제가 발생할 수 있다. */
	WARN_ON(smp_processor_id() != 0);

	// boot_capabilities 가 매개변수이므로 type 이 SCOPE_BOOT_CPU 인
	// cpu caps중에서만 적용된다.(ex. ARM64_HAS_SYSREG_GIC_CPUIF)
	__apply_alternatives(&region, false, &boot_capabilities[0]);
}

#ifdef CONFIG_MODULES
void apply_alternatives_module(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};
	DECLARE_BITMAP(all_capabilities, ARM64_NPATCHABLE);

	bitmap_fill(all_capabilities, ARM64_NPATCHABLE);

	__apply_alternatives(&region, true, &all_capabilities[0]);
}
#endif
