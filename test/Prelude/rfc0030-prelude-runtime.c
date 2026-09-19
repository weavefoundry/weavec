// RFC 0030, sections 10.2, 10.7, 10.9 and 11: the prelude's helpers at run
// time. Each check returns its operand when it holds; each failure traps
// (detected by the signal) in trap and verify modes, and in report mode
// prints `weavec: runtime check failed: <template> at <file>:<line>:<column>`
// once per site through libweavec_rt.a, or aborts under WEAVEC_RT_ABORT=1.
// The term helpers saturate toward failure; the zero-initialisation
// wrappers leave every usable byte zero or written. The same helpers out of
// line, from libweavec_chk.a, behave the same.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_prelude --mode=verify -o %t/verify.h
// RUN: %weavec_prelude --mode=report -o %t/report.h
// RUN: %clang -std=c11 -Wall -Wextra -Werror -O0 -include %t/verify.h %s -o %t/trap0
// RUN: %clang -std=c11 -Wall -Wextra -Werror -O2 -include %t/verify.h %s -o %t/trap2
// RUN: %t/trap0 | FileCheck --check-prefix=PASS %s
// RUN: %t/trap2 | FileCheck --check-prefix=PASS %s
//
// RUN: %t/trap0 nonnull 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap0 span 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap0 violation 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 nonnull 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 nonnull_n 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 nonnull_fn 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 index 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 index_negative 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 span 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 span_before 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 span_overflow 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 len 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 len_r 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 disjoint 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 assert 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 violation 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 strnlen 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_nonnull 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_index 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_span 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_len 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_disjoint 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap2 prv_assert 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// RUN: %clang -std=c11 -Wall -Wextra -Werror -O2 -DREPORT -include %t/report.h %s %weavec_rt -o %t/report
// RUN: %t/report | FileCheck --check-prefix=PASS %s
// RUN: %t/report all 2>&1 | FileCheck --check-prefix=REPORT %s
// RUN: env WEAVEC_RT_ABORT=1 %t/report index 2>&1 | FileCheck --check-prefix=ABORT %s
//
// RUN: %clang -std=c11 -Wall -Wextra -Werror -O2 -DOUT_OF_LINE %s %weavec_chk %weavec_rt -o %t/outofline
// RUN: %t/outofline | FileCheck --check-prefix=LIBRARY %s
// RUN: %t/outofline index 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/outofline report 2>&1 | FileCheck --check-prefix=LIBREPORT %s
//
// A failed check is detected by its signal, which the program catches: an
// uncaught one costs seconds per process for the crash report on macOS.
// TRAPPED: signal: {{SIGTRAP|SIGILL}}
// TRAPPED-NOT: done
//
// PASS: passes: 0 failures
// ABORT: weavec: runtime check failed: index at {{.*}}rfc0030-prelude-runtime.c:{{[0-9]+}}:7
// ABORT-NEXT: signal: SIGABRT
// LIBRARY: library: 0 failures
// LIBREPORT: weavec: runtime check failed: index at library.c:12:34

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define USABLE(p) malloc_size(p)
#else
#include <malloc.h>
#define USABLE(p) malloc_usable_size(p)
#endif

// The report family takes the site; the trap family does not.
#ifdef REPORT
#define AT , __FILE__, __LINE__, 7
#define AT0 __FILE__, __LINE__, 7
#else
#define AT
#define AT0
#endif

static int failures;
#define EXPECT(c)                                                              \
  do {                                                                         \
    if (!(c)) {                                                                \
      printf("FAILED at line %d: %s\n", __LINE__, #c);                         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static volatile uintptr_t sinkValue;
__attribute__((unused)) static void sink(uintptr_t value) { sinkValue = value; }
__attribute__((unused)) static void sinkPointer(const volatile void *p) { sinkValue = (uintptr_t)p; }
// Values the optimiser cannot see through.
__attribute__((unused)) static long long opaque(long long value) {
  volatile long long copy = value;
  return copy;
}
__attribute__((unused)) static void *opaquePointer(void *p) {
  void *volatile copy = p;
  return copy;
}
__attribute__((unused)) static void noop(void) {}

// Names the signal a failed check raised (a trap is SIGTRAP or SIGILL,
// WEAVEC_RT_ABORT's abort SIGABRT) and exits.
static void onSignal(int number) {
  const char *message = number == SIGTRAP  ? "signal: SIGTRAP\n"
                        : number == SIGILL ? "signal: SIGILL\n"
                                           : "signal: SIGABRT\n";
  if (write(2, message, strlen(message)) < 0)
    _exit(1);
  _exit(0);
}

static void catchSignals(void) {
  signal(SIGTRAP, onSignal);
  signal(SIGILL, onSignal);
  signal(SIGABRT, onSignal);
}

// Dirties the whole usable region. Not memset: the fortified one knows only
// the requested size and would fail past it.
__attribute__((unused)) static void fill(void *p, int byte) {
  volatile unsigned char *bytes = p;
  size_t i;
  for (i = 0; i < USABLE(p); ++i)
    bytes[i] = (unsigned char)byte;
}

// Every byte of [from, usable) is zero.
__attribute__((unused)) static int zeroFrom(const void *p, size_t from) {
  const unsigned char *bytes = p;
  size_t i;
  for (i = from; i < USABLE(p); ++i)
    if (bytes[i] != 0)
      return 0;
  return 1;
}

#ifndef OUT_OF_LINE

static void passes(void) {
  int a[8] = {0};
  char buf[16] = "";
  const unsigned long long max = ~0ULL;
  EXPECT(__weavec_chk_nonnull(a AT) == a);
  // Null is allowed with a zero length.
  EXPECT(__weavec_chk_nonnull_n(opaquePointer(0), 0 AT) == 0);
  EXPECT(__weavec_chk_nonnull_fn(noop AT) == noop);
  EXPECT(__weavec_chk_index(3, 8 AT) == 3);
  // A cursor subscript p[-1] inside its object passes; the last element too.
  EXPECT(__weavec_chk_span(&a[2], -1, a, sizeof a, sizeof a[0] AT) == &a[1]);
  EXPECT(__weavec_chk_span(&a[2], 5, a, sizeof a, sizeof a[0] AT) == &a[7]);
  EXPECT(__weavec_chk_len(8, 8 AT) == 8);
  EXPECT(__weavec_chk_len_r(15, sizeof buf AT) == 15);
  EXPECT(__weavec_chk_len_r(-1, sizeof buf AT) == -1);
  EXPECT(__weavec_chk_disjoint(buf, buf + 8, 8 AT) == buf);
  EXPECT(__weavec_chk_disjoint(buf, buf + 1, 0 AT) == buf);
  __weavec_chk_assert(1 AT);
  EXPECT(__weavec_strnlen("abc", 10 AT) == 3);
  EXPECT(__weavec_strnlen("abc", 2 AT) == 2);
#ifndef REPORT
  EXPECT(__weavec_prv_nonnull(a) == a);
  EXPECT(__weavec_prv_index(3, 8) == 3);
  EXPECT(__weavec_prv_span(&a[2], -2, a, sizeof a, sizeof a[0]) == &a[0]);
  EXPECT(__weavec_prv_len(3, 8) == 3);
  EXPECT(__weavec_prv_disjoint(buf, buf + 8, 8) == buf);
  __weavec_prv_assert(1);
#endif

  // Term arithmetic saturates toward failure.
  EXPECT(__weavec_need_s(opaque(-1)) == max);
  EXPECT(__weavec_need_s(5) == 5);
  EXPECT(__weavec_have_s(opaque(-1)) == 0);
  EXPECT(__weavec_have_s(5) == 5);
  EXPECT(__weavec_need_add(max, 1) == max);
  EXPECT(__weavec_need_add(2, 3) == 5);
  EXPECT(__weavec_need_sub(1, 2) == max);
  EXPECT(__weavec_need_sub(max, 5) == max);
  EXPECT(__weavec_need_sub(5, 2) == 3);
  EXPECT(__weavec_need_mul(1ULL << 40, 1ULL << 40) == max);
  EXPECT(__weavec_need_mul(6, 7) == 42);
  EXPECT(__weavec_have_add(max, 1) == 0);
  EXPECT(__weavec_have_add(2, 3) == 5);
  EXPECT(__weavec_have_sub(1, 2) == 0);
  EXPECT(__weavec_have_sub(5, 2) == 3);
  EXPECT(__weavec_have_mul(1ULL << 40, 1ULL << 40) == 0);
  EXPECT(__weavec_have_mul(6, 7) == 42);

  // Zero-initialisation: every usable byte is zero or written.
  unsigned char *p = __weavec_malloc_zero(10);
  EXPECT(p && zeroFrom(p, 0));
  fill(p, 0xab);
  p = __weavec_realloc_zero(p, 4);
  EXPECT(p && p[0] == 0xab && p[3] == 0xab && zeroFrom(p, 4));
  fill(p, 0xcd);
  const size_t old = USABLE(p);
  p = __weavec_realloc_zero(p, 4096);
  EXPECT(p && p[0] == 0xcd && p[old - 1] == 0xcd && zeroFrom(p, old));
  p = __weavec_realloc_zero(p, 0);
  EXPECT(p && zeroFrom(p, 0));
  free(p);
  p = __weavec_calloc_zero(3, 5);
  EXPECT(p && zeroFrom(p, 0));
  free(p);
  p = malloc(8);
  errno = 0;
  EXPECT(__weavec_reallocarray_zero(p, (size_t)1 << 62, 8) == 0);
  EXPECT(errno == ENOMEM);
  p = __weavec_reallocarray_zero(p, 3, 4);
  EXPECT(p && zeroFrom(p, 12));
  free(p);
  char *s = __weavec_strdup_zero("hello");
  EXPECT(s && strcmp(s, "hello") == 0 && zeroFrom(s, 6));
  free(s);
  s = __weavec_strndup_zero("hello", 3);
  EXPECT(s && strcmp(s, "hel") == 0 && zeroFrom(s, 4));
  free(s);
  // What getline or asprintf leaves: three bytes and a terminator.
  s = malloc(64);
  fill(s, 'x');
  memcpy(s, "abc", 4);
  EXPECT(__weavec_zero_line(3, &s) == 3 && zeroFrom(s, 4));
  EXPECT(__weavec_zero_line(-1, &s) == -1);
  free(s);
  void *aligned = __weavec_aligned_zero(aligned_alloc, 64, 128);
  EXPECT(aligned && (uintptr_t)aligned % 64 == 0 && zeroFrom(aligned, 0));
  free(aligned);
  aligned = 0;
  EXPECT(__weavec_posix_memalign_zero(posix_memalign, &aligned, 64, 100) == 0);
  EXPECT(aligned && zeroFrom(aligned, 0));
  free(aligned);
}

static int want(const char *which, const char *name) {
  return strcmp(which, "all") == 0 || strcmp(which, name) == 0;
}

// One failing call per case, in the order the report check lines expect.
static void fails(const char *which) {
  int a[8] = {0};
  char buf[16] = "";
  void (*volatile nullFunction)(void) = 0;
  int i;
  if (want(which, "nonnull"))
    // REPORT: weavec: runtime check failed: nonnull at {{.*}}rfc0030-prelude-runtime.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_nonnull(opaquePointer(0) AT));
  if (want(which, "nonnull_n"))
    // REPORT-NEXT: failed: nonnull at {{.*}}.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_nonnull_n(opaquePointer(0), 4 AT));
  if (want(which, "nonnull_fn"))
    // REPORT-NEXT: failed: nonnull at {{.*}}.c:[[#@LINE+1]]:7
    sink((uintptr_t)__weavec_chk_nonnull_fn(nullFunction AT));
  if (want(which, "index"))
    // REPORT-NEXT: failed: index at {{.*}}.c:[[#@LINE+1]]:7
    sink(__weavec_chk_index((unsigned long long)opaque(8), 8 AT));
  if (want(which, "index_negative"))
    // REPORT-NEXT: failed: index at {{.*}}.c:[[#@LINE+1]]:7
    sink(__weavec_chk_index((unsigned long long)opaque(-1), 8 AT));
  if (want(which, "span"))
    // REPORT-NEXT: failed: span at {{.*}}.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_span(&a[2], opaque(6), a, sizeof a, 4 AT));
  if (want(which, "span_before"))
    // REPORT-NEXT: failed: span at {{.*}}.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_span(&a[2], opaque(-3), a, sizeof a, 4 AT));
  if (want(which, "span_overflow"))
    // REPORT-NEXT: failed: span at {{.*}}.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_span(a, opaque(1LL << 62), a, sizeof a, 4 AT));
  if (want(which, "len"))
    // REPORT-NEXT: failed: len at {{.*}}.c:[[#@LINE+1]]:7
    sink(__weavec_chk_len((unsigned long long)opaque(17), sizeof buf AT));
  if (want(which, "len_r"))
    // REPORT-NEXT: failed: len at {{.*}}.c:[[#@LINE+1]]:7
    sink((uintptr_t)__weavec_chk_len_r((int)opaque(16), sizeof buf AT));
  if (want(which, "disjoint"))
    // REPORT-NEXT: failed: disjoint at {{.*}}.c:[[#@LINE+1]]:7
    sinkPointer(__weavec_chk_disjoint(buf, buf + opaque(4), 8 AT));
  if (want(which, "assert"))
    // REPORT-NEXT: failed: assert at {{.*}}.c:[[#@LINE+1]]:7
    __weavec_chk_assert((int)opaque(0) AT);
  if (want(which, "violation"))
    // REPORT-NEXT: failed: violation at {{.*}}.c:[[#@LINE+1]]:7
    __weavec_chk_violation(AT0);
  if (want(which, "strnlen"))
    // REPORT-NEXT: failed: nonnull at {{.*}}.c:[[#@LINE+1]]:7
    sink(__weavec_strnlen(opaquePointer(0), 4 AT));
  // One site failing three times is reported once.
  for (i = 0; i < 3; ++i)
    if (want(which, "repeat"))
      // REPORT-NEXT: failed: index at {{.*}}.c:[[#@LINE+1]]:7
      sink(__weavec_chk_index((unsigned long long)opaque(9), 8 AT));
  // REPORT-NEXT: done
#ifndef REPORT
  if (want(which, "prv_nonnull"))
    sinkPointer(__weavec_prv_nonnull(opaquePointer(0)));
  if (want(which, "prv_index"))
    sink(__weavec_prv_index((unsigned long long)opaque(8), 8));
  if (want(which, "prv_span"))
    sinkPointer(__weavec_prv_span(&a[2], opaque(6), a, sizeof a, 4));
  if (want(which, "prv_len"))
    sink(__weavec_prv_len((unsigned long long)opaque(17), sizeof buf));
  if (want(which, "prv_disjoint"))
    sinkPointer(__weavec_prv_disjoint(buf, buf + opaque(4), 8));
  if (want(which, "prv_assert"))
    __weavec_prv_assert((int)opaque(0));
#endif
}

int main(int argc, char **argv) {
  catchSignals();
  if (argc > 1) {
    fails(argv[1]);
    fprintf(stderr, "done\n");
    return 0;
  }
  passes();
  printf("passes: %d failures\n", failures);
  return failures != 0;
}

#else // OUT_OF_LINE

// libweavec_chk.a's helpers, declared as for a precompiled-header build.
void *__weavec_chk_nonnull(const volatile void *p);
unsigned long long __weavec_chk_index(unsigned long long i,
                                      unsigned long long n);
unsigned long long __weavec_chk_index_report(unsigned long long i,
                                             unsigned long long n,
                                             const char *file, unsigned line,
                                             unsigned column);
unsigned long long __weavec_need_mul(unsigned long long a,
                                     unsigned long long b);
void *__weavec_malloc_zero(size_t n);
void *__weavec_realloc_zero(void *p, size_t n);

int main(int argc, char **argv) {
  catchSignals();
  if (argc > 1 && strcmp(argv[1], "index") == 0)
    sink(__weavec_chk_index((unsigned long long)opaque(9), 8));
  if (argc > 1 && strcmp(argv[1], "report") == 0)
    sink(__weavec_chk_index_report((unsigned long long)opaque(9), 8,
                                   "library.c", 12, 34));
  if (argc > 1)
    return 0;
  int a[4];
  EXPECT(__weavec_chk_nonnull(a) == a);
  EXPECT(__weavec_chk_index(3, 8) == 3);
  EXPECT(__weavec_need_mul(1ULL << 40, 1ULL << 40) == ~0ULL);
  unsigned char *p = __weavec_malloc_zero(10);
  EXPECT(p && zeroFrom(p, 0));
  fill(p, 0xab);
  p = __weavec_realloc_zero(p, 3);
  EXPECT(p && p[0] == 0xab && zeroFrom(p, 3));
  free(p);
  printf("library: %d failures\n", failures);
  return failures != 0;
}

#endif
