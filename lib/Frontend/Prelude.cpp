//===- Prelude.cpp - The check prelude ------------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Prelude.h"

#include "weavec/Frontend/CheckEmitter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>

namespace weavec::frontend {

// The helpers are written once, with markers that `render` replaces:
//
//   $A     the attributes: `static __inline__ __attribute__((...)) `, or
//          nothing out of line
//   $N     the family prefix, `__weavec_chk_` or `__weavec_prv_`
//   $S     the name suffix: `_report` for out-of-line report helpers
//   $P     report mode's trailing parameters (`, const char *file, ...`)
//   $F(t)  the failure for template `t`: a trap or a report
//   $U     the usable-size query
//   $R     the frame of the function a stack helper is called from
//   $G     the guard kind bits of the family (RFC 0034 §2.5)
//   $D     report mode's site record, declared in a guard's body
//   $L     the site argument of `__weavec_rt_guard`: `&site` or `0`
//   $B     a variadic wrapper's attributes: `static __attribute__((...)) `
//          (it cannot be inlined), or nothing out of line
//   $Q     report mode's trailing arguments (`, file, line, column`)
//   $W     a variadic wrapper's last named parameter
//
// Only block comments: `//` is an error under -std=c89 -pedantic-errors.

/// The check templates; each can fail. The verify family has all but
/// `violation`, which `CheckHelpersString` holds.
static constexpr llvm::StringLiteral CheckHelpers = R"C(
$A void *$Nnonnull$S(const volatile void *p$P) {
  if (__builtin_expect(p == 0, 0))
    $F(nonnull);
  return (void *)p;
}
/* nonnull, zero-length form: null is allowed when n == 0. */
$A void *$Nnonnull_n$S(const volatile void *p, unsigned long long n$P) {
  if (__builtin_expect(p == 0 && n != 0, 0))
    $F(nonnull);
  return (void *)p;
}
/* nonnull, function-pointer form: the callee of an indirect call. */
$A void (*$Nnonnull_fn$S(void (*f)(void)$P))(void) {
  if (__builtin_expect(f == 0, 0))
    $F(nonnull);
  return f;
}
$A unsigned long long $Nindex$S(unsigned long long i, unsigned long long n$P) {
  if (__builtin_expect(i >= n, 0))
    $F(index);
  return i;
}
/* The element p[i] of `width` bytes lies inside [base, base + bytes).
 * The address is computed in integers; p + i is never formed unchecked. */
$A void *$Nspan$S(const volatile void *p, long long i, const volatile void *base,
                  unsigned long long bytes, unsigned long long width$P) {
  long long off = (long long)((unsigned long long)p - (unsigned long long)base),
            step = 0, at = 0;
  if (__builtin_expect(p == 0 || __builtin_mul_overflow(i, (long long)width, &step) ||
                       __builtin_add_overflow(off, step, &at) || at < 0 ||
                       bytes < width || (unsigned long long)at > bytes - width, 0))
    $F(span);
  return (char *)p + step;
}
$A unsigned long long $Nlen$S(unsigned long long need, unsigned long long have$P) {
  if (__builtin_expect(need > have, 0))
    $F(len);
  return need;
}
/* len, result form: the snprintf lowering of sprintf. */
$A int $Nlen_r$S(int written, unsigned long long have$P) {
  if (__builtin_expect(written >= 0 && (unsigned long long)written >= have, 0))
    $F(len);
  return written;
}
$A void *$Ndisjoint$S(const volatile void *d, const volatile void *s,
                      unsigned long long n$P) {
  unsigned long long a = (unsigned long long)d, b = (unsigned long long)s;
  /* (A copy onto itself leaves the bytes as they were: RFC 0033, 1.) */
  if (__builtin_expect(n != 0 && a != b && (a < b ? b - a < n : a - b < n), 0))
    $F(disjoint);
  return (void *)d;
}
$A void $Nassert$S(int c$P) {
  if (__builtin_expect(!c, 0))
    $F(assert);
}
)C";

/// The bounded string length of `str` requirements: `__weavec_chk_*`
/// only.
static constexpr llvm::StringLiteral CheckHelpersString = R"C(
/* Bounded string length for str requirements; fails on null. */
$A unsigned long long __weavec_strnlen$S(const char *s, unsigned long long max$P) {
  unsigned long long n = 0;
  if (__builtin_expect(s == 0, 0)) {
    $F(nonnull);
    return 0;
  }
  while (n < max && s[n])
    ++n;
  return n;
}
)C";

/// Term arithmetic: never C's own, which wraps unsigned values, is
/// undefined on signed overflow and turns a negative count into a huge one.
static constexpr llvm::StringLiteral TermHelpers = R"C(
/* Term arithmetic. A term inside arithmetic is exact over `long long`, as
 * the program computes it, and only its result enters through need_s or
 * have_s: a negative result is the maximum as a need and 0 as a have. A
 * need (a length, an offset, a requirement) is over-approximated: on
 * overflow it saturates to LLONG_MAX and stays there. A have (an extent)
 * is under-approximated: on overflow it saturates to LLONG_MIN and stays
 * there. The right operand of a subtraction is computed in the other
 * direction. A 64-bit unsigned leaf enters through term_u. Every direction
 * fails closed. */
$A unsigned long long __weavec_need_s(long long v) {
  return v < 0 ? ~(unsigned long long)0 : (unsigned long long)v;
}
$A unsigned long long __weavec_have_s(long long v) {
  return v < 0 ? 0 : (unsigned long long)v;
}
$A long long __weavec_term_u(unsigned long long v) {
  const unsigned long long max = ~(unsigned long long)0 >> 1;
  return (long long)(v > max ? max : v);
}
$A long long __weavec_need_add(long long a, long long b) {
  const long long max = (long long)(~(unsigned long long)0 >> 1);
  long long r;
  return a == max || b == max || __builtin_add_overflow(a, b, &r) ? max : r;
}
$A long long __weavec_need_sub(long long a, long long b) {
  const long long max = (long long)(~(unsigned long long)0 >> 1);
  long long r;
  return a == max || b == -max - 1 || __builtin_sub_overflow(a, b, &r) ? max
                                                                       : r;
}
$A long long __weavec_need_mul(long long a, long long b) {
  const long long max = (long long)(~(unsigned long long)0 >> 1);
  long long r;
  return a == max || b == max || __builtin_mul_overflow(a, b, &r) ? max : r;
}
$A long long __weavec_need_div(long long a, long long k) {
  const long long max = (long long)(~(unsigned long long)0 >> 1);
  return a == max ? max : a / k - (a % k < 0);
}
$A long long __weavec_have_add(long long a, long long b) {
  const long long min = -(long long)(~(unsigned long long)0 >> 1) - 1;
  long long r;
  return a == min || b == min || __builtin_add_overflow(a, b, &r) ? min : r;
}
$A long long __weavec_have_sub(long long a, long long b) {
  const long long min = -(long long)(~(unsigned long long)0 >> 1) - 1;
  long long r;
  return a == min || b == -(min + 1) || __builtin_sub_overflow(a, b, &r) ? min
                                                                         : r;
}
$A long long __weavec_have_mul(long long a, long long b) {
  const long long min = -(long long)(~(unsigned long long)0 >> 1) - 1;
  long long r;
  return a == min || b == min || __builtin_mul_overflow(a, b, &r) ? min : r;
}
$A long long __weavec_have_div(long long a, long long k) {
  const long long min = -(long long)(~(unsigned long long)0 >> 1) - 1;
  return a == min ? min : a / k - (a % k < 0);
}
)C";

/// RFC 0034 §1.1: the guards the backend expands. Only the out-of-line
/// prelude (`libweavec_chk.a`) defines them, as the slow path does them; the
/// inline prelude declares them (`GuardDeclarations`).
static constexpr llvm::StringLiteral GuardBodies = R"C(
/* Guards (RFC 0032, section 3; RFC 0033, section 4; RFC 0034, section 2.5).
 * The `width` bytes at q = p + off + i * step lie inside the live tracked
 * object q points into, as the runtime answers for q and p. */
$A void *$Nobject$S(const volatile void *p, long long i, unsigned long long step,
                    unsigned long long off, unsigned long long width$P) {
  long long delta = 0;
  unsigned kind = $G;$D
  if (__builtin_mul_overflow(i, (long long)step, &delta))
    kind |= 4u;
  __weavec_rt_guard((const void *)p, (const char *)p + off + delta, width, kind, $L);
  return (char *)p + delta;
}
/* p does not point into a dead tracked object. */
$A void *$Nlive$S(const volatile void *p$P) {
  unsigned kind = 1u | $G;$D
  __weavec_rt_guard((const void *)p, (const void *)p, 0, kind, $L);
  return (void *)p;
}
)C";

/// The same guards' declarations, in the inline prelude.
static constexpr llvm::StringLiteral GuardDeclarations = R"C(
extern void *$Nobject$S(const volatile void *, long long, unsigned long long,
                        unsigned long long, unsigned long long$P);
extern void *$Nlive$S(const volatile void *$P);
)C";

/// RFC 0032, section 3: the guards of a call's arguments, and of a release,
/// which ask the runtime's object table out of line.
static constexpr llvm::StringLiteral CallGuardHelpers = R"C(
/* object, for a call's argument: `need` bytes from p; none needs no object. */
$A void *$Nobject_n$S(const volatile void *p, unsigned long long need$P) {
  if (__builtin_expect(need != 0 && __weavec_rt_object((const void *)p, 0, 0, 0, need), 0))
    $F(object);
  return (void *)p;
}
/* object, wrapping a call's length argument (RFC 0033, section 5): `need`
 * bytes from p, which the call reads or writes; the length is returned. */
$A unsigned long long $Nobject_l$S(const volatile void *p, unsigned long long need$P) {
  if (__builtin_expect(need != 0 && __weavec_rt_object((const void *)p, 0, 0, 0, need), 0))
    $F(object);
  return need;
}
/* object, for a string argument: its terminator lies inside p's object. */
$A char *$Nobject_s$S(const char *p$P) {
  if (__builtin_expect(__weavec_rt_string(p), 0))
    $F(object);
  return (char *)p;
}
/* p may be released: null, the start of a live heap object, or untracked.
 * Where a failure is only reported, the release is skipped. */
$A void *$Nrelease$S(const volatile void *p$P) {
  int bad = __weavec_rt_release_ok((const void *)p);
  if (__builtin_expect(bad, 0)) {
    $F(release);
    return (void *)0;
  }
  return (void *)p;
}
)C";

/// RFC 0034, section 5.2: the checked wrappers of the library calls whose
/// need no term can state. `__weavec_chk_*` only, with the runtime.
static constexpr llvm::StringLiteral WrapperHelpers = R"C(
/* Checked wrappers (RFC 0034, section 5.2). A library call whose need no
 * term states calls its wrapper, which computes the need at run time and
 * then makes the call. `cap` is a fortified call's object size (the maximum
 * without one); `what` holds the checks planned: 1 the room behind the
 * destination, 2 the operands' disjointness, 4 the bytes behind the
 * source. A copy of n bytes from s to d (n is the maximum for a string
 * without a terminator in its object): */
$A void $Ncopy_w$S(const void *d, const void *s, unsigned long long n,
                   unsigned long long cap, unsigned what$P) {
  unsigned long long a = (unsigned long long)d, b = (unsigned long long)s;
  if (__builtin_expect(n > cap || ((what & 1u) != 0 &&
                                   (n == ~(unsigned long long)0 ||
                                    __weavec_rt_object(d, 0, 0, 0, n))), 0))
    $F(object);
  else if (__builtin_expect((what & 4u) != 0 && n != 0 &&
                            __weavec_rt_object(s, 0, 0, 0, n), 0))
    $F(object);
  else if (__builtin_expect((what & 2u) != 0 && n != 0 && a != b &&
                            (a < b ? b - a < n : a - b < n), 0))
    $F(disjoint);
}
$A char *$Nstrcpy$S(char *d, const char *s, unsigned long long cap,
                    unsigned what$P) {
  unsigned long long n = (what & 1u) != 0 ? __weavec_rt_strlen(s) : __builtin_strlen(s);
  $Ncopy_w$S(d, s, n == ~(unsigned long long)0 ? n : n + 1, cap, what$Q);
  return __builtin_strcpy(d, s);
}
$A char *$Nstpcpy$S(char *d, const char *s, unsigned long long cap,
                    unsigned what$P) {
  unsigned long long n = (what & 1u) != 0 ? __weavec_rt_strlen(s) : __builtin_strlen(s);
  $Ncopy_w$S(d, s, n == ~(unsigned long long)0 ? n : n + 1, cap, what$Q);
  return __builtin_stpcpy(d, s);
}
/* strcat needs both lengths behind d. */
$A char *$Nstrcat$S(char *d, const char *s, unsigned long long cap,
                    unsigned what$P) {
  unsigned long long n = 0;
  if (__builtin_add_overflow(__weavec_rt_strlen(d), __weavec_rt_strlen(s), &n) ||
      __builtin_add_overflow(n, 1u, &n))
    n = ~(unsigned long long)0;
  $Ncopy_w$S(d, s, n, cap, what & 1u$Q);
  return __builtin_strcat(d, s);
}
$A void *$Nmemcpy$S(void *d, const volatile void *s, __typeof__(sizeof 0) n,
                    unsigned long long cap, unsigned what$P) {
  $Ncopy_w$S(d, (const void *)s, n, cap, what$Q);
  return __builtin_memcpy(d, (const void *)s, n);
}
$A void *$Nmemmove$S(void *d, const volatile void *s, __typeof__(sizeof 0) n,
                     unsigned long long cap, unsigned what$P) {
  $Ncopy_w$S(d, (const void *)s, n, cap, what & 5u$Q);
  return __builtin_memmove(d, (const void *)s, n);
}
/* sprintf writes through the bounded writer, as much as the room behind d
 * holds; an output that did not fit fails. */
$A int $Nvsprintf$S(char *d, const char *f, __builtin_va_list ap,
                    unsigned long long cap, unsigned what$P) {
  unsigned long long room = (what & 1u) != 0 ? __weavec_rt_room(d) : ~(unsigned long long)0;
  int r;
  if (room > cap)
    room = cap;
  if (room > 0x7fffffff)
    room = 0x7fffffff;
  r = __builtin_vsnprintf(d, room, f, ap);
  if (__builtin_expect(r >= 0 && (unsigned long long)r >= room, 0))
    $F(object);
  return r;
}
$B int $Nsprintf$S(char *d, const char *f, unsigned long long cap,
                   unsigned what$P, ...) {
  __builtin_va_list ap;
  int r;
  __builtin_va_start(ap, $W);
  r = $Nvsprintf$S(d, f, ap, cap, what$Q);
  __builtin_va_end(ap);
  return r;
}
)C";

/// RFC 0032, sections 4.2 and 6: the stack-object helpers and the string
/// length a guard's need reads. `__weavec_chk_*` builds only.
static constexpr llvm::StringLiteral RuntimeHelpers = R"C(
/* The length of the string at s, read inside s's own object. */
$A unsigned long long __weavec_obj_strlen(const char *s) {
  return __weavec_rt_strlen(s);
}
/* Stack objects (RFC 0032, section 4.2; RFC 0034, section 4): a local a guard
 * can reach is entered where it is declared and left where its scope ends.
 * One on a granule (the compiler aligns them) is written into the shadow
 * here: its last granule reads 0x40 + its bytes there, each granule r
 * before it 0x80 + r up to 48, and 0x80 + 48 + c above, where r is at least
 * 2^(c + 4); the granules of one c are written at once. Anything else, or
 * one whose shadow the mask wraps, asks the runtime. */
$A void *__weavec_stack_enter(void *base, __typeof__(sizeof 0) size, int flags) {
  unsigned long long a = (unsigned long long)base, mask = __weavec_rt_heap.mask;
  unsigned long long g = a >> 4, n = ((unsigned long long)size + 15) >> 4;
  if ((a & 15) == 0 && mask != 0 && size != 0 && ((g + n) & mask) > (g & mask)) {
    unsigned char *at = (unsigned char *)__weavec_rt_heap.shadow + (g & mask);
    unsigned long long r = n - 1, c = 15, low;
    while (r > 48) {
      while (c > 1 && (1ull << (c + 4)) > r)
        --c;
      low = c > 1 ? 1ull << (c + 4) : 49;
      __builtin_memset(at, (int)(0x80 + 48 + c), r - low + 1);
      at += r - low + 1;
      r = low - 1;
    }
    for (; r > 0; --r)
      *at++ = (unsigned char)(0x80 + r);
    /* A last granule it shares with a live mixed object stays mixed. */
    if ((size & 15) == 0 || *at != 0xFC)
      *at = (unsigned char)(0x40 + ((unsigned long long)size - 16 * (n - 1)));
    /* One past it belongs to it: an untracked granule after it says so,
     * unless the frame has unnamed storage (flag 1), which may start there. */
    if ((size & 15) == 0 && (flags & 1) == 0 && at[1] == 0)
      at[1] = 0xFB;
    return base;
  }
  return __weavec_rt_stack_enter(base, size, $R, flags);
}
$A void __weavec_stack_leave(void **slot) {
  unsigned long long a = (unsigned long long)*slot, mask = __weavec_rt_heap.mask;
  if ((a & 15) == 0 && mask != 0) {
    unsigned char *shadow = (unsigned char *)__weavec_rt_heap.shadow;
    unsigned long long g = a >> 4, run;
    unsigned char v;
    /* Up to and including its last granule; nothing when it was not
     * written here. A run byte's least r stays inside the object. */
    while ((v = shadow[g & mask]) > 0x80 && v <= 0x80 + 63) {
      run = v <= 0x80 + 48 ? v - 0x80u : 1ull << (v - 0x80 - 48 + 4);
      if (((g + run) & mask) > (g & mask)) {
        __builtin_memset(shadow + (g & mask), 0, run);
        g += run;
      } else {
        shadow[g++ & mask] = 0;
      }
    }
    if (v > 0x40 && v <= 0x50) {
      shadow[g & mask] = 0;
      if (shadow[(g + 1) & mask] == 0xFB)
        shadow[(g + 1) & mask] = 0;
    }
    return;
  }
  __weavec_rt_stack_leave(*slot, $R);
}
/* After a returns-twice call returns, deeper frames are gone. */
$A int __weavec_stack_rewind(int result) {
  /* (Its first return abandoned no frame.) */
  if (result != 0)
    __weavec_rt_stack_rewind($R);
  return result;
}
)C";

/// The runtime entry points the helpers above call.
static constexpr llvm::StringLiteral RuntimeDeclarations =
    "struct __weavec_rt_heap_t { unsigned long long base, bytes, shadow, "
    "mask; };\n"
    "extern struct __weavec_rt_heap_t __weavec_rt_heap;\n"
    "struct __weavec_rt_site { const char *file; unsigned line, column; };\n"
    "extern void __weavec_rt_guard(const void *, const void *, unsigned long "
    "long, unsigned, const struct __weavec_rt_site *);\n"
    "extern int __weavec_rt_object(const void *, long long, unsigned long "
    "long, "
    "unsigned long long, unsigned long long);\n"
    "extern int __weavec_rt_string(const char *);\n"
    "extern unsigned long long __weavec_rt_strlen(const char *);\n"
    "extern unsigned long long __weavec_rt_room(const void *);\n"
    "extern int __weavec_rt_release_ok(const void *);\n"
    "extern void *__weavec_rt_stack_enter(void *, __typeof__(sizeof 0), void "
    "*, "
    "int);\n"
    "extern void __weavec_rt_stack_leave(void *, void *);\n"
    "extern void __weavec_rt_stack_rewind(void *);\n"
    "extern void __weavec_rt_trapping(void);\n";

/// Section 11. Only builtins and the usable-size query: the prelude must
/// not declare a library function a unit may declare differently.
static constexpr llvm::StringLiteral ZeroInitHelpers = R"C(
/* Zero-initialisation: every byte of a lowered block's usable region is
 * zero or was written by the program, so no realloc can expose a stale
 * pointer. */
$A void *__weavec_zero_tail(void *p, __typeof__(sizeof 0) written) {
  __typeof__(sizeof 0) usable;
  if (p != 0) {
    usable = $U(p);
    if (written < usable)
      __builtin_memset((char *)p + written, 0, usable - written);
  }
  return p;
}
/* calloc avoids touching the fresh pages of a large allocation. */
$A void *__weavec_malloc_zero(__typeof__(sizeof 0) n) {
  return __weavec_zero_tail(__builtin_calloc(1, n), n);
}
$A void *__weavec_calloc_zero(__typeof__(sizeof 0) m, __typeof__(sizeof 0) n) {
  return __weavec_zero_tail(__builtin_calloc(m, n), m * n);
}
/* A zero size becomes one, so realloc(p, 0) behaves the same on every C
 * library. Zeroing from min(n, old) clears both the tail a shrink leaves
 * and the part of a moved block realloc did not copy. */
$A void *__weavec_realloc_zero(void *p, __typeof__(sizeof 0) n) {
  __typeof__(sizeof 0) old = p != 0 ? $U(p) : 0;
  return __weavec_zero_tail(__builtin_realloc(p, n != 0 ? n : 1),
                            n < old ? n : old);
}
/* An overflowing product fails as the allocator fails a request it cannot
 * meet: null, errno ENOMEM, the block untouched. */
$A void *__weavec_reallocarray_zero(void *p, __typeof__(sizeof 0) m,
                                    __typeof__(sizeof 0) n) {
  __typeof__(sizeof 0) bytes;
  if (__builtin_mul_overflow(m, n, &bytes))
    return __builtin_realloc(p, ~(__typeof__(sizeof 0))0);
  return __weavec_realloc_zero(p, bytes);
}
/* aligned_alloc and memalign have no builtin: the rewrite passes the
 * program's own callee, and the whole usable region is zeroed. */
$A void *__weavec_aligned_zero(void *(*f)(__typeof__(sizeof 0), __typeof__(sizeof 0)),
                               __typeof__(sizeof 0) a, __typeof__(sizeof 0) n) {
  return __weavec_zero_tail(f(a, n), 0);
}
$A int __weavec_posix_memalign_zero(
    int (*f)(void **, __typeof__(sizeof 0), __typeof__(sizeof 0)), void **slot,
    __typeof__(sizeof 0) a, __typeof__(sizeof 0) n) {
  int rc = f(slot, a, n);
  if (rc == 0)
    (void)__weavec_zero_tail(*slot, 0);
  return rc;
}
/* Rows whose call writes a string: zero from after its terminator. */
$A char *__weavec_zero_string(char *p) {
  return p != 0 ? (char *)__weavec_zero_tail(p, __builtin_strlen(p) + 1) : p;
}
$A char *__weavec_strdup_zero(const char *s) {
  return __weavec_zero_string(__builtin_strdup(s));
}
$A char *__weavec_strndup_zero(const char *s, __typeof__(sizeof 0) n) {
  return __weavec_zero_string(__builtin_strndup(s, n));
}
/* getline, getdelim, asprintf, vasprintf: the call wrote n bytes and a
 * terminator to *slot. `slot` is evaluated again, so the rewrite applies
 * only when it has no side effects. */
$A long long __weavec_zero_line(long long n, char **slot) {
  if (n >= 0 && slot != 0)
    (void)__weavec_zero_tail(*slot, (__typeof__(sizeof 0))n + 1);
  return n;
}
/* alloca(n) is lowered at the site, as
 * __builtin_memset(__builtin_alloca(n), 0, n): an alloca in a helper
 * would be released when the inlined helper returns. */
)C";

/// Section 11: `p = malloc` takes the address of a static, non-inline
/// wrapper with the callee's own signature. Inline form only: out of line
/// the wrappers above are real functions already.
static constexpr llvm::StringLiteral AddressWrappers = R"C(
/* Address-taken allocators: static, non-inline, with the same signature. */
static __attribute__((unused, nodebug)) void *__weavec_malloc_zero_fn(__typeof__(sizeof 0) n) {
  return __weavec_malloc_zero(n);
}
static __attribute__((unused, nodebug)) void *__weavec_calloc_zero_fn(__typeof__(sizeof 0) m, __typeof__(sizeof 0) n) {
  return __weavec_calloc_zero(m, n);
}
static __attribute__((unused, nodebug)) void *__weavec_realloc_zero_fn(void *p, __typeof__(sizeof 0) n) {
  return __weavec_realloc_zero(p, n);
}
static __attribute__((unused, nodebug)) void *__weavec_reallocarray_zero_fn(void *p, __typeof__(sizeof 0) m, __typeof__(sizeof 0) n) {
  return __weavec_reallocarray_zero(p, m, n);
}
static __attribute__((unused, nodebug)) char *__weavec_strdup_zero_fn(const char *s) {
  return __weavec_strdup_zero(s);
}
static __attribute__((unused, nodebug)) char *__weavec_strndup_zero_fn(const char *s, __typeof__(sizeof 0) n) {
  return __weavec_strndup_zero(s, n);
}
)C";

namespace {

/// How one family of helpers is spelled.
struct Style {
  llvm::StringRef attributes;
  llvm::StringRef prefix;
  llvm::StringRef category;
  llvm::StringRef suffix;
  bool report = false;
  bool outOfLine = false;
  bool verboseTrap = true;
  /// RFC 0033 §6.1: the runtime is linked, so a trap first makes sure it
  /// ends the program.
  bool trapping = false;
  llvm::StringRef usable;
  /// RFC 0034 §2.5: the guard kind bits of the family.
  llvm::StringRef guardKind = "0u";
};

} // namespace

static constexpr llvm::StringLiteral InlineAttributes =
    "static __inline__ __attribute__((always_inline, nodebug, unused))";
static constexpr llvm::StringLiteral ReportParameters =
    "const char *file, unsigned line, unsigned column";

static void failure(llvm::StringRef reason, const Style &style,
                    std::string &out) {
  if (style.report)
    out += ("__weavec_rt_report(\"" + reason + "\", file, line, column)").str();
  else if (style.outOfLine)
    out += ("WEAVEC_CHK_TRAP(\"" + style.category + "\", \"" + reason + "\")")
               .str();
  else if (style.verboseTrap)
    out += ((style.trapping ? "(__weavec_rt_trapping(), " : "(") +
            ("__builtin_verbose_trap(\"" + style.category + "\", \"" + reason +
             "\"))"))
               .str();
  else
    out += style.trapping ? "(__weavec_rt_trapping(), __builtin_trap())"
                          : "__builtin_trap()";
}

/// Replaces the markers of `text` (see the top of the file).
static std::string render(llvm::StringRef text, const Style &style) {
  std::string out;
  out.reserve(text.size() + (text.size() / 4));
  while (!text.empty()) {
    const std::size_t marker = text.find('$');
    out += text.take_front(marker).str();
    if (marker == llvm::StringRef::npos)
      break;
    text = text.drop_front(marker + 1);
    const char kind = text.front();
    text = text.drop_front();
    switch (kind) {
    case 'A':
      out += style.attributes.str();
      // Out of line there are no attributes, and no space after them.
      if (style.attributes.empty())
        text.consume_front(" ");
      break;
    case 'N':
      out += style.prefix.str();
      break;
    case 'S':
      out += style.suffix.str();
      break;
    case 'P':
      if (style.report)
        out += (", " + ReportParameters).str();
      break;
    case 'U':
      out += style.usable.str();
      break;
    case 'G':
      out += style.guardKind.str();
      break;
    case 'D':
      if (style.report)
        out += "\n  struct __weavec_rt_site site;\n  site.file = file;\n"
               "  site.line = line;\n  site.column = column;";
      break;
    case 'L':
      out += style.report ? "&site" : "0";
      break;
    case 'B':
      if (style.outOfLine)
        text.consume_front(" ");
      else
        out += "static __attribute__((unused, nodebug))";
      break;
    case 'Q':
      if (style.report)
        out += ", file, line, column";
      break;
    case 'W':
      out += style.report ? "column" : "what";
      break;
    case 'R':
      // Inlined, a helper's frame is its caller's. Out of line it would be
      // the helper's own, so units built with a precompiled header register
      // no stack object (`CheckEmitter`).
      out += "__builtin_frame_address(0)";
      break;
    case 'F': {
      const std::size_t close = text.find(')');
      failure(text.substr(1, close - 1), style, out);
      text = text.drop_front(close + 1);
      break;
    }
    default:
      llvm_unreachable("unknown prelude marker");
    }
  }
  return out;
}

static llvm::StringRef usableQueryName(UsableSizeQuery query) {
  switch (query) {
  case UsableSizeQuery::None:
    return "";
  case UsableSizeQuery::MallocSize:
    return "malloc_size";
  case UsableSizeQuery::MallocUsableSize:
  case UsableSizeQuery::MallocUsableSizeConst:
    return "malloc_usable_size";
  }
  llvm_unreachable("unknown usable-size query");
}

/// Compatible with the declaration in the target's system headers.
static llvm::StringRef usableQueryDeclaration(UsableSizeQuery query) {
  switch (query) {
  case UsableSizeQuery::None:
    return "";
  case UsableSizeQuery::MallocSize:
    return "extern __typeof__(sizeof 0) malloc_size(const void *);\n";
  case UsableSizeQuery::MallocUsableSize:
    return "extern __typeof__(sizeof 0) malloc_usable_size(void *);\n";
  case UsableSizeQuery::MallocUsableSizeConst:
    return "extern __typeof__(sizeof 0) malloc_usable_size(const void *);\n";
  }
  llvm_unreachable("unknown usable-size query");
}

/// The C spelling of a helper's parameter or result type; `name` is the
/// declarator.
static std::string declarator(HelperSignature::Type type,
                              llvm::StringRef name) {
  using Type = HelperSignature::Type;
  const auto spelled = [&](llvm::StringRef base) {
    return (base + (name.empty() ? "" : " ") + name).str();
  };
  switch (type) {
  case Type::Void:
    return spelled("void");
  case Type::Int:
    return spelled("int");
  case Type::Unsigned:
    return spelled("unsigned");
  case Type::LongLong:
    return spelled("long long");
  case Type::UnsignedLongLong:
    return spelled("unsigned long long");
  case Type::Size:
    return spelled("__typeof__(sizeof 0)");
  case Type::VoidPointer:
    return ("void *" + name).str();
  case Type::ConstVolatileVoidPointer:
    return ("const volatile void *" + name).str();
  case Type::CharPointer:
    return ("char *" + name).str();
  case Type::ConstCharPointer:
    return ("const char *" + name).str();
  case Type::CharPointerPointer:
    return ("char **" + name).str();
  case Type::VoidPointerPointer:
    return ("void **" + name).str();
  case Type::FunctionPointer:
    return spelled("__weavec_fn_t");
  case Type::VaList:
    return spelled("__builtin_va_list");
  case Type::AlignedAllocator:
  case Type::PosixMemalignFunction:
    break;
  }
  llvm_unreachable("no check helper takes an allocator");
}

/// RFC 0033 *Implementation amendments* (build cost): a copy of each check
/// and guard helper `prelude` defines, `<name>_ool`, that is not inlined. A
/// function with more helper calls than
/// `CheckEmitterOptions::inlinedHelperCalls` calls these: inlined into
/// thousands of sites, the helpers kept the optimiser on one function for
/// minutes.
static std::string outOfLineCopies(llvm::StringRef prelude, bool report) {
  using Type = HelperSignature::Type;
  std::string out = "typedef void (*__weavec_fn_t)(void);\n";
  for (const HelperSignature &helper : CheckEmitter::helperSignatures()) {
    if (!helper.reports || helper.declared || helper.wrapper ||
        !prelude.contains((helper.name + "(").str()))
      continue;
    std::string parameters;
    std::string arguments;
    std::size_t index = 0;
    for (const Type param : helper.params) {
      if (param == Type::Void)
        break;
      const std::string name = "a" + std::to_string(index++);
      parameters += (parameters.empty() ? "" : ", ") + declarator(param, name);
      arguments += (arguments.empty() ? "" : ", ") + name;
    }
    if (report) {
      parameters += (parameters.empty() ? "" : ", ") + ReportParameters.str();
      arguments +=
          arguments.empty() ? "file, line, column" : ", file, line, column";
    }
    out += "static __attribute__((noinline, nodebug, unused)) ";
    out += declarator(helper.result,
                      (helper.name + "_ool(" +
                       (parameters.empty() ? "void" : parameters) + ")")
                          .str());
    out += helper.result == Type::Void ? " { " : " { return ";
    out += (helper.name + "(" + arguments + "); }\n").str();
  }
  return out;
}

std::string buildCheckPrelude(const PreludeOptions &options) {
  if (options.mode == CheckMode::None)
    return {};
  const bool outOfLine = options.form == PreludeForm::OutOfLine;
  const bool report = options.mode == CheckMode::Report;
  Style check;
  check.attributes =
      outOfLine ? llvm::StringRef() : llvm::StringRef(InlineAttributes);
  check.prefix = "__weavec_chk_";
  check.category = "weavec";
  check.suffix = outOfLine && report ? "_report" : "";
  check.report = report;
  check.outOfLine = outOfLine;
  check.verboseTrap = options.verboseTrap;
  check.trapping = options.runtime;
  // RFC 0034 §5.3: with the runtime linked, its own size query, which
  // answers an arena block without asking every zone.
  check.usable = usableQueryName(options.usableSize);
  if (options.runtime)
    check.usable = "__weavec_rt_size";
  if (outOfLine)
    check.usable = "WEAVEC_CHK_USABLE";

  std::string out = "\n/* WeaveC check prelude (RFC 0030, section 10.2): ";
  out += checkModeName(options.mode).str();
  out += outOfLine ? " mode, out of line. */\n" : " mode. */\n";
  if (!outOfLine)
    out += "#pragma clang diagnostic push\n"
           "#pragma clang diagnostic ignored \"-Weverything\"\n";
  if (report)
    out += "extern void __weavec_rt_report(const char *, const char *, "
           "unsigned, unsigned);\n";
  if (options.runtime)
    out += RuntimeDeclarations.str();
  out += render(CheckHelpers, check).substr(1);
  out += render(CheckHelpersString, check).substr(1);
  // RFC 0034 §1.1: inline, the backend's guards are declared under their
  // `_report` names in report mode, as `libweavec_chk.a` defines them.
  const auto guards = [&](const Style &family) {
    if (outOfLine) {
      out += render(GuardBodies, family).substr(1);
    } else {
      Style declared = family;
      declared.suffix = report ? "_report" : "";
      out += render(GuardDeclarations, declared).substr(1);
    }
    out += render(CallGuardHelpers, family).substr(1);
  };
  if (options.runtime) {
    guards(check);
    out += render(WrapperHelpers, check).substr(1);
  }
  if (options.mode == CheckMode::Verify) {
    Style proven = check;
    proven.prefix = "__weavec_prv_";
    proven.category = "weavec.proven";
    proven.guardKind = "8u";
    out += render(CheckHelpers, proven).substr(1);
    if (options.runtime)
      guards(proven);
  }
  // Out of line, the report object holds only the report helpers; the rest
  // come from the trap object of the same archive.
  if (!(outOfLine && report)) {
    out += render(TermHelpers, check).substr(1);
    if (options.runtime)
      out += render(RuntimeHelpers, check).substr(1);
    if (options.zeroInit && outOfLine) {
      out += "#ifdef WEAVEC_CHK_USABLE\n";
      out += render(ZeroInitHelpers, check).substr(1);
      out += "#endif\n";
    } else if (options.zeroInit &&
               options.usableSize != UsableSizeQuery::None) {
      out +=
          options.runtime
              ? "extern __typeof__(sizeof 0) __weavec_rt_size(const void *);\n"
              : usableQueryDeclaration(options.usableSize).str();
      out += render(ZeroInitHelpers, check).substr(1);
      out += AddressWrappers.substr(1).str();
    }
  }
  if (!outOfLine) {
    out += outOfLineCopies(out, report);
    out += "#pragma clang diagnostic pop\n";
  }
  return out;
}

UsableSizeQuery usableSizeQueryFor(const llvm::Triple &triple) {
  if (triple.isOSDarwin())
    return UsableSizeQuery::MallocSize;
  if (triple.isAndroid() || triple.isOSFreeBSD())
    return UsableSizeQuery::MallocUsableSizeConst;
  if (triple.isOSLinux() && (triple.isGNUEnvironment() || triple.isMusl()))
    return UsableSizeQuery::MallocUsableSize;
  return UsableSizeQuery::None;
}

std::optional<CheckMode> parseCheckMode(llvm::StringRef name) {
  for (const CheckMode mode :
       {CheckMode::Trap, CheckMode::Report, CheckMode::Verify, CheckMode::None})
    if (name == checkModeName(mode))
      return mode;
  return std::nullopt;
}

llvm::StringRef checkModeName(CheckMode mode) {
  switch (mode) {
  case CheckMode::Trap:
    return "trap";
  case CheckMode::Report:
    return "report";
  case CheckMode::Verify:
    return "verify";
  case CheckMode::None:
    return "none";
  }
  llvm_unreachable("unknown check mode");
}

static constexpr std::array<llvm::StringLiteral, 9> Templates{
    "nonnull", "index",  "span", "len",    "disjoint",
    "assert",  "object", "live", "release"};

llvm::ArrayRef<llvm::StringLiteral> checkTemplates() {
  return Templates;
}

} // namespace weavec::frontend
