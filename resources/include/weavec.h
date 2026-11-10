/*===- weavec.h - WeaveC source annotations for C ----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* weavec-cc guards every memory access of the C it compiles. WEAVEC_UNSAFE
|* marks the code that must not be guarded: a function (placed before its
|* definition) or a block (placed before a compound statement) whose accesses
|* read past an object on purpose (a word-at-a-time scan, a conservative
|* stack scan). The macro expands to nothing on compilers that do not
|* understand Clang's `annotate` attribute, so the code stays buildable with
|* any C toolchain.
|*
|*   #include <weavec.h>
|*
|*   WEAVEC_UNSAFE void scan(void) { ... }   // the body is an unsafe region
|*   WEAVEC_UNSAFE { ... }                   // or just a block
|*
|* A function marked no_sanitize("address") is an unsafe region too, as it is
|* to AddressSanitizer.
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_H
#define WEAVEC_H

#define WEAVEC_H_VERSION_MAJOR 2
#define WEAVEC_H_VERSION_MINOR 0

#if defined(__has_attribute)
#if __has_attribute(annotate)
#define WEAVEC_ANNOTATE_(text) __attribute__((annotate(text)))
#endif
#endif

#ifndef WEAVEC_ANNOTATE_
#define WEAVEC_ANNOTATE_(text)
#endif

/* The spelling must match weavec::frontend::UnsafeAnnotation. */

/**
 * Makes a function body (when placed before its definition) or a block (when
 * placed before a compound statement) an unsafe region: weavec-cc does not
 * guard its memory accesses or check its array indexes at run time. Keep
 * regions small and document the invariant that makes the code sound.
 */
#define WEAVEC_UNSAFE WEAVEC_ANNOTATE_("weavec.unsafe")

/*===-- Safe C (RFC 0041) -------------------------------------------------===*
 *
 * A safe function is checked against Safe C's rules when it is compiled and
 * rejected with an error naming the rule and the fix when it breaks one;
 * once accepted, its accesses are checked against the bounds its pointers
 * carry. The guide is docs/pages/guides/safe-c.md. The spellings below must
 * match weavec::frontend::SafeAnnotation and its neighbors
 * (include/weavec/Frontend/SafeCheck.h). Under a compiler other than Clang
 * they expand to nothing, so safe C is plain C.
 *
 *   WEAVEC_SAFE int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
 *   WEAVEC_SAFE size_t length(const char *WEAVEC_NULL_TERMINATED s);
 *   WEAVEC_SAFE void push(struct list *l, struct node *WEAVEC_ESCAPES n);
 *   WEAVEC_SAFE char *find(char *s WEAVEC_LIFETIMEBOUND, char c);
 */

/** Makes a function definition safe (RFC 0041 §1). */
#define WEAVEC_SAFE WEAVEC_ANNOTATE_("weavec.safe")
/** In a unit compiled with -fweavec-safe: keeps a function guarded. */
#define WEAVEC_GUARDED WEAVEC_ANNOTATE_("weavec.guarded")

/* A build that must not see `[[…]]` before C23 (-std=c99 -pedantic-errors)
 * defines WEAVEC_TYPE_ANNOTATE_(...) as nothing before including this. */
#if defined(__clang__)
#ifndef WEAVEC_TYPE_ANNOTATE_
#define WEAVEC_TYPE_ANNOTATE_(...) [[clang::annotate_type(__VA_ARGS__)]]
#endif
#define WEAVEC_LIFETIMEBOUND __attribute__((lifetimebound))
#else
#ifndef WEAVEC_TYPE_ANNOTATE_
#define WEAVEC_TYPE_ANNOTATE_(...)
#endif
#define WEAVEC_LIFETIMEBOUND
#endif

/**
 * After the `*` of a pointer type: the pointer points to `n` elements, `n`
 * being another parameter (or field) by name, or an integer constant.
 */
#define WEAVEC_COUNTED_BY(n) WEAVEC_TYPE_ANNOTATE_("weavec.counted_by", #n)
/** As WEAVEC_COUNTED_BY, or null whatever `n` is. */
#define WEAVEC_COUNTED_BY_OR_NULL(n)                                           \
  WEAVEC_TYPE_ANNOTATE_("weavec.counted_by_or_null", #n)
/** The pointer points to `n` bytes. */
#define WEAVEC_SIZED_BY(n) WEAVEC_TYPE_ANNOTATE_("weavec.sized_by", #n)
/** As WEAVEC_SIZED_BY, or null whatever `n` is. */
#define WEAVEC_SIZED_BY_OR_NULL(n)                                             \
  WEAVEC_TYPE_ANNOTATE_("weavec.sized_by_or_null", #n)
/** The pointer points to the elements before `e`, another parameter. */
#define WEAVEC_ENDED_BY(e) WEAVEC_TYPE_ANNOTATE_("weavec.ended_by", #e)
/** The pointer points to a string: its bytes up to and with its NUL. */
#define WEAVEC_NULL_TERMINATED WEAVEC_TYPE_ANNOTATE_("weavec.null_terminated")
/**
 * After the `*` of a parameter's type: the function may keep the pointer
 * after it returns, so the argument must not point into a stack.
 */
#define WEAVEC_ESCAPES WEAVEC_TYPE_ANNOTATE_("weavec.escapes")
/**
 * After the `*` of a field's type: the field may hold a borrowed pointer
 * (a safe function's parameter) when its struct is a local variable; a
 * pointer loaded from it is borrowed.
 */
#define WEAVEC_BORROWED WEAVEC_TYPE_ANNOTATE_("weavec.borrowed")

#endif /* WEAVEC_H */
