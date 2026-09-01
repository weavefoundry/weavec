/*===- weavec.h - WeaveC source annotations for C ----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* Lightweight annotations that let you guide WeaveC's ownership inference
|* where it cannot prove safety on its own. Every macro expands to nothing on
|* compilers that do not understand Clang's `annotate` attribute, so annotated
|* code stays buildable with any C toolchain.
|*
|*   #include <weavec.h>
|*
|*   struct buffer *WEAVEC_OWNED buffer_new(size_t n);
|*   void buffer_free(struct buffer *WEAVEC_OWNED b);
|*   size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);
|*   void buffer_push(struct buffer *WEAVEC_MUT b, int v);
|*
|*   WEAVEC_UNSAFE void poke(void) { ... }   // opt a function out of checking
|*   WEAVEC_UNSAFE { ... }                   // or just a block
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_H
#define WEAVEC_H

#define WEAVEC_H_VERSION_MAJOR 0
#define WEAVEC_H_VERSION_MINOR 1

#if defined(__has_attribute)
#if __has_attribute(annotate)
#define WEAVEC_ANNOTATE_(text) __attribute__((annotate(text)))
#endif
#endif

#ifndef WEAVEC_ANNOTATE_
#define WEAVEC_ANNOTATE_(text)
#endif

/* The spellings below must match weavec::analysis::spelling. */

/** The pointer uniquely owns its referent and must release it exactly once. */
#define WEAVEC_OWNED WEAVEC_ANNOTATE_("weavec.owned")

/** A shared, read-only borrow; the referent outlives the borrow. */
#define WEAVEC_BORROWED WEAVEC_ANNOTATE_("weavec.borrowed")

/** An exclusive, mutable borrow; no other access may occur while it lives. */
#define WEAVEC_MUT WEAVEC_ANNOTATE_("weavec.mut_borrowed")

/**
 * Opts a function (when placed before its declaration) or a block (when
 * placed before a compound statement) out of WeaveC's checks. Use sparingly
 * and document the invariant that makes the code sound.
 */
#define WEAVEC_UNSAFE WEAVEC_ANNOTATE_("weavec.unsafe")

/** Non-zero when the translation unit is being processed by WeaveC. */
#if defined(__WEAVEC__)
#define WEAVEC_ENABLED 1
#else
#define WEAVEC_ENABLED 0
#endif

#endif /* WEAVEC_H */
