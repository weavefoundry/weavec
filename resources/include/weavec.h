/*===- weavec.h - WeaveC source annotations for C ----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* Lightweight annotations that let you guide WeaveC's ownership inference
|* where it cannot prove safety on its own. They are inputs to WeaveC's
|* advisory analysis (`weavec`, `weavec-cc -fweavec-diagnose`) and do not
|* change the generated code, except WEAVEC_UNSAFE: weavec-cc guards every
|* memory access at run time outside an unsafe region, whatever the other
|* annotations say. Every macro expands to nothing on compilers that do not
|* understand Clang's `annotate` attribute, so annotated code stays buildable
|* with any C toolchain.
|*
|*   #include <weavec.h>
|*
|*   struct buffer *WEAVEC_OWNED buffer_new(size_t n);
|*   void buffer_free(struct buffer *WEAVEC_OWNED b);
|*   size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);
|*   void buffer_push(struct buffer *WEAVEC_MUT b, int v);
|*   void *WEAVEC_RAW map_pages(size_t n);   // no ownership guarantee
|*   struct node *WEAVEC_NULLABLE find(struct tree *t, int key);
|*   void log_line(const char *WEAVEC_NONNULL msg);
|*
|*   typedef void (*dtor_t)(void *WEAVEC_OWNED);   // callbacks, too
|*
|*   struct obj { int WEAVEC_REFCOUNT rc; ... };   // reference counting
|*   struct obj *obj_ref(struct obj *WEAVEC_RETAINS o);
|*   void obj_unref(struct obj *WEAVEC_RELEASES o);
|*   FILE *WEAVEC_OWNED WEAVEC_OWNED_BY(fclose) open_log(void);
|*
|*   WEAVEC_UNSAFE void poke(void) { ... }   // the body is an unsafe region
|*   WEAVEC_UNSAFE { ... }                   // or just a block
|*
|*   void fill(char *WEAVEC_COUNTED_BY(len) buf, size_t len);   // extents
|*   size_t sum(const int *WEAVEC_ENDED_BY(end) p, const int *end);
|*   size_t name_len(const char *WEAVEC_STRING name);
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_H
#define WEAVEC_H

#define WEAVEC_H_VERSION_MAJOR 0
#define WEAVEC_H_VERSION_MINOR 10

#if defined(__has_attribute)
#if __has_attribute(annotate)
#define WEAVEC_ANNOTATE_(text) __attribute__((annotate(text)))
#endif
#endif

#ifndef WEAVEC_ANNOTATE_
#define WEAVEC_ANNOTATE_(text)
#endif

/* The spellings below must match weavec::analysis::spelling. */

/**
 * The pointer uniquely owns its referent and must release it exactly once.
 * On a struct field, the object owns the referent: releasing the object
 * while the field still holds one is reported as a leak.
 */
#define WEAVEC_OWNED WEAVEC_ANNOTATE_("weavec.owned")

/** A shared, read-only borrow; the referent outlives the borrow. */
#define WEAVEC_BORROWED WEAVEC_ANNOTATE_("weavec.borrowed")

/** An exclusive, mutable borrow; no other access may occur while it lives. */
#define WEAVEC_MUT WEAVEC_ANNOTATE_("weavec.mut_borrowed")

/**
 * A raw pointer: no ownership guarantee at all. The analysis reports
 * dereferencing or releasing it outside a WEAVEC_UNSAFE function or block.
 * Copying, comparing and passing it to another WEAVEC_RAW parameter are
 * fine.
 */
#define WEAVEC_RAW WEAVEC_ANNOTATE_("weavec.raw")

/**
 * Makes a function body (when placed before its definition) or a block (when
 * placed before a compound statement) an unsafe region: weavec-cc does not
 * guard its memory accesses or check its array indexes at run time (as for a
 * function marked no_sanitize("address")), and the analysis trusts its raw
 * pointers and its spatial and null operations. The analysis still tracks
 * temporal state (frees, moves) there and still reports possible temporal
 * findings and definite violations; what the region does to the surrounding
 * code is checked there. The only annotation that changes the generated
 * code. Keep regions small and document the invariant that makes the code
 * sound.
 */
#define WEAVEC_UNSAFE WEAVEC_ANNOTATE_("weavec.unsafe")

/**
 * The pointer may be null. On a parameter, the body treats it as possibly
 * null until it tests it, and callers may pass null; on a return type,
 * callers must test the result before dereferencing it; on a variable or
 * field, every load is treated as possibly null. Does not change ownership.
 */
#define WEAVEC_NULLABLE WEAVEC_ANNOTATE_("weavec.nullable")

/**
 * The pointer is never null. On a parameter, a definitely null argument is
 * reported; on a return type, the result needs no test; on a variable or
 * field, loads are never reported. Does not change ownership.
 */
#define WEAVEC_NONNULL WEAVEC_ANNOTATE_("weavec.nonnull")

/**
 * On a pointer parameter: the callee takes a reference on the argument's
 * object (increments its reference count). The caller's pointer gains a
 * share, which the next copy of it carries away (`q = obj_ref(p)` leaves
 * `p` and `q` each holding one). Does not consume the argument.
 */
#define WEAVEC_RETAINS WEAVEC_ANNOTATE_("weavec.retains")

/**
 * On a pointer parameter: the callee releases one reference on the
 * argument's object. The argument's name is dead afterwards, as after
 * `free`; other pointers holding their own reference are untouched.
 */
#define WEAVEC_RELEASES WEAVEC_ANNOTATE_("weavec.releases")

/**
 * On an integer struct field: the field is a reference count. A reference
 * taken through it (`o->rc++`, a WEAVEC_RETAINS callee) and never released
 * is reported as a leak, even when no releasing function is in view.
 */
#define WEAVEC_REFCOUNT WEAVEC_ANNOTATE_("weavec.refcount")

/**
 * Next to WEAVEC_OWNED on a parameter or return type: the referent must be
 * released with `f` (`WEAVEC_OWNED WEAVEC_OWNED_BY(fclose)`), and releasing
 * it with anything else is reported as a mismatched release.
 */
#define WEAVEC_OWNED_BY(f) WEAVEC_ANNOTATE_("weavec.family." #f)

/**
 * On a pointer parameter or field: at least `n` elements (bytes for void
 * and character pointees) are accessible; `n` names a sibling parameter or
 * field, in any position (`void fill(char *WEAVEC_COUNTED_BY(len) buf,
 * size_t len)`, `struct buf { char *WEAVEC_COUNTED_BY(cap) data; size_t
 * cap; }`). The analysis checks the kind at every call and every store and
 * proves accesses through the pointer against it. weavec-cc guards those
 * accesses against the object the pointer points into, not against `n`.
 */
#define WEAVEC_COUNTED_BY(n) WEAVEC_ANNOTATE_("weavec.counted_by." #n)

/**
 * The same as WEAVEC_COUNTED_BY(n): `n` counts elements (bytes for void and
 * character pointees), unlike Clang's byte-counting `sized_by`. New code
 * should use WEAVEC_COUNTED_BY.
 */
#define WEAVEC_SIZED_BY(n) WEAVEC_ANNOTATE_("weavec.sized_by." #n)

/**
 * On a pointer parameter or field: `[p, q)` lies in one object, where `q`
 * names a sibling pointer parameter or field (`size_t sum(const int
 * *WEAVEC_ENDED_BY(end) p, const int *end)`).
 */
#define WEAVEC_ENDED_BY(q) WEAVEC_ANNOTATE_("weavec.ended_by." #q)

/**
 * On a pointer parameter, field or return type: the pointer is
 * NUL-terminated within its object (a zero element lies at or after it).
 */
#define WEAVEC_STRING WEAVEC_ANNOTATE_("weavec.string")

/** Non-zero when the translation unit is being processed by WeaveC. */
#if defined(__WEAVEC__)
#define WEAVEC_ENABLED 1
#else
#define WEAVEC_ENABLED 0
#endif

/**
 * States that `expr` holds here, and the analysis assumes it from here on,
 * as if the code below were inside `if (expr)`: `WEAVEC_ASSUME(len <= cap)`
 * lets the checker prove an access in bounds when the invariant that makes
 * it so is not visible in the function. When the analysis refutes `expr`,
 * that is a `contradicted-assumption`. Nothing checks it at run time, and
 * no guard depends on it: it is a statement to the analysis only. `expr`
 * must be side-effect free; under other compilers it is an unevaluated
 * operand and is not compiled at all.
 */
#if WEAVEC_ENABLED
WEAVEC_ANNOTATE_("weavec.assume")
static inline void weavec_assume_(int condition) {
  (void)condition;
}
#define WEAVEC_ASSUME(expr) weavec_assume_((expr) != 0)
#else
/* Unevaluated, so `expr` is neither run nor an unused-variable warning. */
#define WEAVEC_ASSUME(expr) ((void)sizeof((expr) != 0))
#endif

#endif /* WEAVEC_H */
