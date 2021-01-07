/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BUILD_BUG_H
#define _LINUX_BUILD_BUG_H

#include <linux/compiler.h>

#ifdef __CHECKER__
#define BUILD_BUG_ON_ZERO(e) (0)
#else /* __CHECKER__ */
/*
 * 조건이 참이면 컴파일 에러를 강제한다. 그러나 결과(값 0 와 size_t 유형)도 생성하므로
 * 표현식은 예. 구조체 initializer 에 사용될 수 있다.(또는 다른 곳에서는 쉼표식이
 * 허용되지 않음?)
 */
// 매크로명이 BUILD BUG OR ZERO가 되어야 한다. 조건 e가 참이면 컴파일 에러.
// 아니면 0으로 대체된다.
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:(-!!(e)); }))
#endif /* __CHECKER__ */

/* Force a compilation error if a constant expression is not a power of 2 */
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)	\
	BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)			\
	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))

/*
 * BUILD_BUG_ON_INVALID는 컴파일러가 표현식의 유효성을 검사하도록 한다.
 * 그러나 해당 식에 부작용이 있더라도 코드를 생성하는 것은 피한다.
 */
// FIXME. overflow를 검사하는지 잘 모르겠다.
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((__force long)(e))))

/**
 * BUILD_BUG_ON_MSG - break compile if a condition is true & emit supplied
 *		      error message.
 * @condition: the condition which the compiler should know is false.
 *
 * See BUILD_BUG_ON for description.
 */
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

/**
 * BUILD_BUG_ON - break compile if a condition is true.
 * @condition: the condition which the compiler should know is false.
 *
 * If you have some code which relies on certain constants being equal, or
 * some other compile-time-evaluated condition, you should use BUILD_BUG_ON to
 * detect if someone changes it.
 */
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)

/**
 * BUILD_BUG - break compile if used.
 *
 * If you have some code that you expect the compiler to eliminate at
 * build time, you should use BUILD_BUG to detect if it is
 * unexpectedly used.
 */
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")

/**
 * static_assert - check integer constant expression at build time
 *
 * static_assert() is a wrapper for the C11 _Static_assert, with a
 * little macro magic to make the message optional (defaulting to the
 * stringification of the tested expression).
 *
 * Contrary to BUILD_BUG_ON(), static_assert() can be used at global
 * scope, but requires the expression to be an integer constant
 * expression (i.e., it is not enough that __builtin_constant_p() is
 * true for expr).
 *
 * Also note that BUILD_BUG_ON() fails the build if the condition is
 * true, while static_assert() fails the build if the expression is
 * false.
 */
#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)

#endif	/* _LINUX_BUILD_BUG_H */
