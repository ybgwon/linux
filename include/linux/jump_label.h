/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_JUMP_LABEL_H
#define _LINUX_JUMP_LABEL_H

/*
 * Jump label 지원
 *
 * Copyright (C) 2009-2012 Jason Baron <jbaron@redhat.com>
 * Copyright (C) 2011-2012 Red Hat, Inc., Peter Zijlstra
 *
 * 폐지 예정 API:
 *
 * 'struct static_key' 를 직접사용하는 것은, 이제 구식이다. 따라서
 * static_key_{true,false}()도 마찬가지이다. 예. 다음을 사용하지 마세요
 *
 * struct static_key false = STATIC_KEY_INIT_FALSE;
 * struct static_key true = STATIC_KEY_INIT_TRUE;
 * static_key_true()
 * static_key_false()
 *
 * 업데이트되어 대체된 API는 다음과 같다
 *
 * DEFINE_STATIC_KEY_TRUE(key);
 * DEFINE_STATIC_KEY_FALSE(key);
 * DEFINE_STATIC_KEY_ARRAY_TRUE(keys, count);
 * DEFINE_STATIC_KEY_ARRAY_FALSE(keys, count);
 * static_branch_likely()
 * static_branch_unlikely()
 *
 * Jump labels는 self-modifying 코드를 이용하여 동적 branches를 만드는
 * interface를 제공한다. 툴체인가 아키텍쳐가 지원한다는 가정하에
 * "DEFINE_STATIC_KEY_FALSE(key)"로 false로 초기화된 키를 정의하면
 * "if (static_branch_unlikely(&key))"문은 무조건 분기이다.
 * (기본값은 false이며  - 그리고 true 블록은 라인 밖이다.)
 * 유사하게, "DEFINE_STATIC_KEY_TRUE(key)"를 통해 true로 초기화하는 키를
 * 정의 할 수 있고 그 키를 같은 "if (static_branch_unlikely(&key))"에 사용하면
 * 이 경우 라인 밖의 true branch로 향하는 무조건 분기가 만들어진다.
 * true 나 false로 초기화 된 키는 static_branch_unlikely()
 * 와 static_branch_likely() 둘 다에서 사용할수 있다.
 *
 * runtime에 static_branch_enable()을 호출하여 키를 true로 설정하거나
 * static_branch_disable()를 이용하여 false로 설정하여 분기 타겟을
 * 바꿀수 있다. 분기의 방향이 이 호출에 의해 바뀌면 run-time에 분기 타겟을
 * no-op -> jump or jump -> no-op 변환을 통해 변경한다. 예를 들면
 * "if (static_branch_unlikely(&key))" 문에서 false로 초기화 된 키는
 * 키를 true로 설정하려면 true 브랜치의 줄 외부로 jump를 패치해야한다.
 *
 * static_branch_{enable,disable}에 더하여, static_branch_{inc,dec} 함수를 통해
 * 키 카운터나 분기 방향을 참조할 수 있다. static_branch_inc() 는 '더 참으로 만들기'
 * static_branch_dec() 는 '더 거짓으로 만들기'로 생각할 수 있다.
 *
 * 이것은 코드 변경에 의존하므로 분기 변경 함수는 절대 slow paths 로 간주 되어야 한다.
 * (머신 전체 동기화 등.) 반면에 영향을 받은 분기는 무조건이므로
 * runtime overhead(실시간 간접비)는 절대적으로 최소가 된다. 기본(off) 에서
 * 총 영향은 적절한 크기의 단일 NOP 이다. on 의 경우는 블록 바깥으로 jump 하여
 * 패치할 것이다.
 *
 * 제어가 사용자영역에 바로 노출되어 있을때, 상단한 성능 저하를 유발할 수 있는 고빈도
 * 코드 변경을 피하기 위해 감소(decrement)를 지연시키는 것이 바람직하다.
 * static_key_deferred 구조체와 static_key_slow_dec_deferred()함수가
 * 이를 위해 제공된다.
 *
 * 툴체인 이나 아키텍쳐 지원이 부족한 static keys는 간단 조건 분기로 돌아간다.
 *
 * 추가 정보: Documentation/static-keys.txt
 */

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/compiler.h>

extern bool static_key_initialized;

#define STATIC_KEY_CHECK_USE(key) WARN(!static_key_initialized,		      \
				    "%s(): static key '%pS' used before call to jump_label_init()", \
				    __func__, (key))

#ifdef CONFIG_JUMP_LABEL

struct static_key {
	atomic_t enabled;
/*
 * 주의:
 *   익명 unions이 옛날 컴파일러에 동작하도록 하기위해 정적 초기화는
 *   브라켓([])이 필요하다.
 *   이것은 initializers를 사용하여 구조체 순서에 대한 종속성을 만든다.
 *   만약 어떤 필드가 추가되면 STATIC_KEY_INIT_TRUE and STATIC_KEY_INIT_FALSE는
 *   수정해야 할 수 있다.
 *
 * bit 0 => 1 키가 처음에 true인 경우
 *	    0 처음에 false인 경우
 * bit 1 => 1 구조체 static_key_mod 를 가리킬 때
 *	    0 구조체 struct jump_entry 를 가리킬 때
 */
	union {
		unsigned long type;
		struct jump_entry *entries;
		struct static_key_mod *next;
	};
};

#else
struct static_key {
	atomic_t enabled;
};
#endif	/* CONFIG_JUMP_LABEL */
#endif /* __ASSEMBLY__ */

#ifdef CONFIG_JUMP_LABEL
#include <asm/jump_label.h>

#ifndef __ASSEMBLY__
#ifdef CONFIG_HAVE_ARCH_JUMP_LABEL_RELATIVE

struct jump_entry {
	s32 code; 	// 코드 상대 주소
	s32 target;	// target 상대 주소
	long key;	// KASLR 아래에서는 core 커널로 부터 멀리 떨어져 있다.
};

static inline unsigned long jump_entry_code(const struct jump_entry *entry)
{
	return (unsigned long)&entry->code + entry->code;
}

static inline unsigned long jump_entry_target(const struct jump_entry *entry)
{
	return (unsigned long)&entry->target + entry->target;
}

static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	long offset = entry->key & ~3L;

	return (struct static_key *)((unsigned long)&entry->key + offset);
}

#else

static inline unsigned long jump_entry_code(const struct jump_entry *entry)
{
	return entry->code;
}

static inline unsigned long jump_entry_target(const struct jump_entry *entry)
{
	return entry->target;
}

static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	return (struct static_key *)((unsigned long)entry->key & ~3UL);
}

#endif

static inline bool jump_entry_is_branch(const struct jump_entry *entry)
{
	return (unsigned long)entry->key & 1UL;
}

static inline bool jump_entry_is_init(const struct jump_entry *entry)
{
	return (unsigned long)entry->key & 2UL;
}

static inline void jump_entry_set_init(struct jump_entry *entry)
{
	entry->key |= 2;
}

#endif
#endif

#ifndef __ASSEMBLY__

enum jump_label_type {
	JUMP_LABEL_NOP = 0,
	JUMP_LABEL_JMP,
};

struct module;

#ifdef CONFIG_JUMP_LABEL

#define JUMP_TYPE_FALSE		0UL
#define JUMP_TYPE_TRUE		1UL
#define JUMP_TYPE_LINKED	2UL
#define JUMP_TYPE_MASK		3UL

static __always_inline bool static_key_false(struct static_key *key)
{
	return arch_static_branch(key, false);
}

static __always_inline bool static_key_true(struct static_key *key)
{
	return !arch_static_branch(key, true);
}

extern struct jump_entry __start___jump_table[];
extern struct jump_entry __stop___jump_table[];

extern void jump_label_init(void);
extern void jump_label_lock(void);
extern void jump_label_unlock(void);
extern void arch_jump_label_transform(struct jump_entry *entry,
				      enum jump_label_type type);
extern void arch_jump_label_transform_static(struct jump_entry *entry,
					     enum jump_label_type type);
extern int jump_label_text_reserved(void *start, void *end);
extern void static_key_slow_inc(struct static_key *key);
extern void static_key_slow_dec(struct static_key *key);
extern void static_key_slow_inc_cpuslocked(struct static_key *key);
extern void static_key_slow_dec_cpuslocked(struct static_key *key);
extern void jump_label_apply_nops(struct module *mod);
extern int static_key_count(struct static_key *key);
extern void static_key_enable(struct static_key *key);
extern void static_key_disable(struct static_key *key);
extern void static_key_enable_cpuslocked(struct static_key *key);
extern void static_key_disable_cpuslocked(struct static_key *key);

/*
 * We should be using ATOMIC_INIT() for initializing .enabled, but
 * the inclusion of atomic.h is problematic for inclusion of jump_label.h
 * in 'low-level' headers. Thus, we are initializing .enabled with a
 * raw value, but have added a BUILD_BUG_ON() to catch any issues in
 * jump_label_init() see: kernel/jump_label.c.
 */
#define STATIC_KEY_INIT_TRUE					\
	{ .enabled = { 1 },					\
	  { .entries = (void *)JUMP_TYPE_TRUE } }
#define STATIC_KEY_INIT_FALSE					\
	{ .enabled = { 0 },					\
	  { .entries = (void *)JUMP_TYPE_FALSE } }

#else  /* !CONFIG_JUMP_LABEL */

#include <linux/atomic.h>
#include <linux/bug.h>

static inline int static_key_count(struct static_key *key)
{
	return atomic_read(&key->enabled);
}

static __always_inline void jump_label_init(void)
{
	static_key_initialized = true;
}

static __always_inline bool static_key_false(struct static_key *key)
{
	if (unlikely(static_key_count(key) > 0))
		return true;
	return false;
}

static __always_inline bool static_key_true(struct static_key *key)
{
	if (likely(static_key_count(key) > 0))
		return true;
	return false;
}

static inline void static_key_slow_inc(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);
	atomic_inc(&key->enabled);
}

static inline void static_key_slow_dec(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);
	atomic_dec(&key->enabled);
}

#define static_key_slow_inc_cpuslocked(key) static_key_slow_inc(key)
#define static_key_slow_dec_cpuslocked(key) static_key_slow_dec(key)

static inline int jump_label_text_reserved(void *start, void *end)
{
	return 0;
}

static inline void jump_label_lock(void) {}
static inline void jump_label_unlock(void) {}

static inline int jump_label_apply_nops(struct module *mod)
{
	return 0;
}

static inline void static_key_enable(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);

	if (atomic_read(&key->enabled) != 0) {
		WARN_ON_ONCE(atomic_read(&key->enabled) != 1);
		return;
	}
	atomic_set(&key->enabled, 1);
}

static inline void static_key_disable(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);

	if (atomic_read(&key->enabled) != 1) {
		WARN_ON_ONCE(atomic_read(&key->enabled) != 0);
		return;
	}
	atomic_set(&key->enabled, 0);
}

#define static_key_enable_cpuslocked(k)		static_key_enable((k))
#define static_key_disable_cpuslocked(k)	static_key_disable((k))

#define STATIC_KEY_INIT_TRUE	{ .enabled = ATOMIC_INIT(1) }
#define STATIC_KEY_INIT_FALSE	{ .enabled = ATOMIC_INIT(0) }

#endif	/* CONFIG_JUMP_LABEL */

#define STATIC_KEY_INIT STATIC_KEY_INIT_FALSE
#define jump_label_enabled static_key_enabled

/* -------------------------------------------------------------------------- */

/*
 * Two type wrappers around static_key, such that we can use compile time
 * type differentiation to emit the right code.
 *
 * All the below code is macros in order to play type games.
 */

struct static_key_true {
	struct static_key key;
};

struct static_key_false {
	struct static_key key;
};

#define STATIC_KEY_TRUE_INIT  (struct static_key_true) { .key = STATIC_KEY_INIT_TRUE,  }
#define STATIC_KEY_FALSE_INIT (struct static_key_false){ .key = STATIC_KEY_INIT_FALSE, }

// struct static_key_true name =
//   (struct static_key_true) { .key = { .enabled = { 1 },
// 				       { .entries = (void *)1UL } }, }
#define DEFINE_STATIC_KEY_TRUE(name)	\
	struct static_key_true name = STATIC_KEY_TRUE_INIT

#define DEFINE_STATIC_KEY_TRUE_RO(name)	\
	struct static_key_true name __ro_after_init = STATIC_KEY_TRUE_INIT

#define DECLARE_STATIC_KEY_TRUE(name)	\
	extern struct static_key_true name

// struct static_key_false name =
//   (struct static_key_false) { .key = { .enabled = { 1 },
// 				        { .entries = (void *)0UL } }, }
#define DEFINE_STATIC_KEY_FALSE(name)	\
	struct static_key_false name = STATIC_KEY_FALSE_INIT

#define DEFINE_STATIC_KEY_FALSE_RO(name)	\
	struct static_key_false name __ro_after_init = STATIC_KEY_FALSE_INIT

#define DECLARE_STATIC_KEY_FALSE(name)	\
	extern struct static_key_false name

#define DEFINE_STATIC_KEY_ARRAY_TRUE(name, count)		\
	struct static_key_true name[count] = {			\
		[0 ... (count) - 1] = STATIC_KEY_TRUE_INIT,	\
	}

#define DEFINE_STATIC_KEY_ARRAY_FALSE(name, count)		\
	struct static_key_false name[count] = {			\
		[0 ... (count) - 1] = STATIC_KEY_FALSE_INIT,	\
	}

extern bool ____wrong_branch_error(void);

#define static_key_enabled(x)							\
({										\
	if (!__builtin_types_compatible_p(typeof(*x), struct static_key) &&	\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_true) &&\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_false))	\
		____wrong_branch_error();					\
	static_key_count((struct static_key *)x) > 0;				\
})

#ifdef CONFIG_JUMP_LABEL

/*
 * 올바른 초기값을 올바른 분기 순서와 결합하여 원하는 결과를 생성한다
 *
 *
 * type\branch|	likely (1)	      |	unlikely (0)
 * -----------+-----------------------+------------------
 *            |                       |
 *  true (1)  |	   ...		      |	   ...
 *            |    NOP		      |	   JMP L
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *            |                       |
 *  false (0) |	   ...		      |	   ...
 *            |    JMP L	      |	   NOP
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *
 * The initial value is encoded in the LSB of static_key::entries,
 * type: 0 = false, 1 = true.
 *
 * The branch type is encoded in the LSB of jump_entry::key,
 * branch: 0 = unlikely, 1 = likely.
 *
 * This gives the following logic table:
 * dynamic 설정이 적용되며 type 열을 둔 것은 runtime에도 enabled 와 type 값은 같이
 * 동시에 존재하므로 경우의 수를 나타내기 위해 표현한 것 같다. 표에서 instruction 설정은
 * enabled ^ branch 값이고 type은 enabled 와 값이 다를 시 static_key_enable 나
 * static_key_disable 함수 등을 통해 초기값이 변경된 경우를 나타낸다고 보면 된다.
 *
 *	enabled	type	branch	  instuction
 * -----------------------------+-----------
 *	0	0	0	| NOP
 *	0	0	1	| JMP
 *	0	1	0	| NOP
 *	0	1	1	| JMP
 *
 *	1	0	0	| JMP
 *	1	0	1	| NOP
 *	1	1	0	| JMP
 *	1	1	1	| NOP
 *
 * Which gives the following functions:
 *
 *   dynamic: instruction = enabled ^ branch
 *   static:  instruction = type ^ branch
 *
 * See jump_label_type() / jump_label_init_type().
 */

// 키 타입에 따라 arch_static_branch(true 타입) 나
// arch_static_branch_jump(false 타입) 를 호출하여 결과를 return 받는다.
// return 받은 bool 값을 반전(!)하여 likely 매크로의 매개변수로하여 반환한다.
// arch_static_branch[_jump] 관련 함수 호출시 두번째 매개변수를 true 로
// 설정하여 static_branch_likely 매크로에서 호출되었음을 표시한다.
#define static_branch_likely(x)							\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = !arch_static_branch(&(x)->key, true);			\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = !arch_static_branch_jump(&(x)->key, true);		\
	else									\
		branch = ____wrong_branch_error();				\
	likely(branch);								\
})

// 키 타입에 따라 arch_static_branch_jump(true 타입) 나
// arch_static_branch(false 타입) 를 호출하여 결과를 return 받는다.
// return 받은 bool 값을 unlikely 매크로의 매개변수로하여 반환한다.
// arch_static_branch[_jump] 관련 함수 호출시 두번째 매개변수를  false 로
// 설정하여 static_branch_unlikely 매크로에서 호출되었음을 표시한다.
#define static_branch_unlikely(x)						\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = arch_static_branch_jump(&(x)->key, false);		\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = arch_static_branch(&(x)->key, false);			\
	else									\
		branch = ____wrong_branch_error();				\
	unlikely(branch);							\
})

#else /* !CONFIG_JUMP_LABEL */

#define static_branch_likely(x)		likely(static_key_enabled(&(x)->key))
#define static_branch_unlikely(x)	unlikely(static_key_enabled(&(x)->key))

#endif /* CONFIG_JUMP_LABEL */

/*
 * Advanced usage; refcount, branch is enabled when: count != 0
 */

#define static_branch_inc(x)		static_key_slow_inc(&(x)->key)
#define static_branch_dec(x)		static_key_slow_dec(&(x)->key)
#define static_branch_inc_cpuslocked(x)	static_key_slow_inc_cpuslocked(&(x)->key)
#define static_branch_dec_cpuslocked(x)	static_key_slow_dec_cpuslocked(&(x)->key)

/*
 * Normal usage; boolean enable/disable.
 */

#define static_branch_enable(x)			static_key_enable(&(x)->key)
#define static_branch_disable(x)		static_key_disable(&(x)->key)
#define static_branch_enable_cpuslocked(x)	static_key_enable_cpuslocked(&(x)->key)
#define static_branch_disable_cpuslocked(x)	static_key_disable_cpuslocked(&(x)->key)

#endif /* __ASSEMBLY__ */

#endif	/* _LINUX_JUMP_LABEL_H */
