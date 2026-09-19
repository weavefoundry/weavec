//===- Prelude.cpp - The check prelude ------------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Prelude.h"

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
//   $V     a parameterless helper's parameter list (`void` or report's)
//   $F(t)  the failure for template `t`: a trap or a report
//   $U     the usable-size query
//
// Only block comments: `//` is an error under -std=c89 -pedantic-errors.

/// The check templates; each can fail. The verify family has all but
/// `violation`, which `CheckHelpersViolation` holds.
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
  if (__builtin_expect(n != 0 && (a < b ? b - a < n : a - b < n), 0))
    $F(disjoint);
  return (void *)d;
}
$A void $Nassert$S(int c$P) {
  if (__builtin_expect(!c, 0))
    $F(assert);
}
)C";

/// The unconditional failure of a lowered violation, and the bounded
/// string length of `str` requirements: `__weavec_chk_*` only.
static constexpr llvm::StringLiteral CheckHelpersViolation = R"C(
/* A lowered violation with no check of its own. */
$A void $Nviolation$S($V) {
  $F(violation);
}
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
/* Term arithmetic over 64 bits. A need (a length, an offset, a
 * requirement) is over-approximated: on overflow it saturates to the
 * maximum and stays there. A have (an extent) is under-approximated: on
 * overflow it saturates to 0, and x - k stops at 0. The right operand of a
 * subtraction is computed in the other direction. A signed leaf enters
 * through need_s or have_s: a negative count is the maximum as a need and
 * 0 as a have. Every direction fails closed. */
$A unsigned long long __weavec_need_s(long long v) {
  return v < 0 ? ~(unsigned long long)0 : (unsigned long long)v;
}
$A unsigned long long __weavec_have_s(long long v) {
  return v < 0 ? 0 : (unsigned long long)v;
}
$A unsigned long long __weavec_need_add(unsigned long long a, unsigned long long b) {
  unsigned long long r;
  return __builtin_add_overflow(a, b, &r) ? ~(unsigned long long)0 : r;
}
$A unsigned long long __weavec_need_sub(unsigned long long a, unsigned long long b) {
  return a == ~(unsigned long long)0 || b > a ? ~(unsigned long long)0 : a - b;
}
$A unsigned long long __weavec_need_mul(unsigned long long a, unsigned long long b) {
  unsigned long long r;
  return __builtin_mul_overflow(a, b, &r) ? ~(unsigned long long)0 : r;
}
$A unsigned long long __weavec_have_add(unsigned long long a, unsigned long long b) {
  unsigned long long r;
  return __builtin_add_overflow(a, b, &r) ? 0 : r;
}
$A unsigned long long __weavec_have_sub(unsigned long long a, unsigned long long b) {
  return b > a ? 0 : a - b;
}
$A unsigned long long __weavec_have_mul(unsigned long long a, unsigned long long b) {
  unsigned long long r;
  return __builtin_mul_overflow(a, b, &r) ? 0 : r;
}
)C";

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
  llvm::StringRef usable;
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
    out += ("__builtin_verbose_trap(\"" + style.category + "\", \"" + reason +
            "\")")
               .str();
  else
    out += "__builtin_trap()";
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
    case 'V':
      out += style.report ? ReportParameters.str() : std::string("void");
      break;
    case 'U':
      out += style.usable.str();
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
  check.usable =
      outOfLine ? "WEAVEC_CHK_USABLE" : usableQueryName(options.usableSize);

  std::string out = "\n/* WeaveC check prelude (RFC 0030, section 10.2): ";
  out += checkModeName(options.mode).str();
  out += outOfLine ? " mode, out of line. */\n" : " mode. */\n";
  if (!outOfLine)
    out += "#pragma clang diagnostic push\n"
           "#pragma clang diagnostic ignored \"-Weverything\"\n";
  if (report)
    out += "extern void __weavec_rt_report(const char *, const char *, "
           "unsigned, unsigned);\n";
  out += render(CheckHelpers, check).substr(1);
  out += render(CheckHelpersViolation, check).substr(1);
  if (options.mode == CheckMode::Verify) {
    Style proven = check;
    proven.prefix = "__weavec_prv_";
    proven.category = "weavec.proven";
    out += render(CheckHelpers, proven).substr(1);
  }
  // Out of line, the report object holds only the report helpers; the rest
  // come from the trap object of the same archive.
  if (!(outOfLine && report)) {
    out += render(TermHelpers, check).substr(1);
    if (options.zeroInit && outOfLine) {
      out += "#ifdef WEAVEC_CHK_USABLE\n";
      out += render(ZeroInitHelpers, check).substr(1);
      out += "#endif\n";
    } else if (options.zeroInit &&
               options.usableSize != UsableSizeQuery::None) {
      out += usableQueryDeclaration(options.usableSize).str();
      out += render(ZeroInitHelpers, check).substr(1);
    }
  }
  if (!outOfLine)
    out += "#pragma clang diagnostic pop\n";
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

static constexpr std::array<llvm::StringLiteral, 7> Templates{
    "nonnull", "index", "span", "len", "disjoint", "assert", "violation"};

llvm::ArrayRef<llvm::StringLiteral> checkTemplates() {
  return Templates;
}

} // namespace weavec::frontend
