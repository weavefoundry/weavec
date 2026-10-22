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

#define WEAVEC_H_VERSION_MAJOR 1
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

#endif /* WEAVEC_H */
