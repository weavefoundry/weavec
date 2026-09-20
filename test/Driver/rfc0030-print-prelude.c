// RFC 0030 §10.9, §16: `weavec-cc -fweavec-print-prelude` prints the check
// prelude of the -fweavec-checks mode, zero-initialisation helpers included
// unless -fno-weavec-zero-init, with the usable-size query of the target,
// and exits without compiling anything. `=out-of-line` prints the external
// definitions libweavec_chk.a is built from.
//
// RUN: %weavec_cc -fweavec-print-prelude | FileCheck --check-prefix=TRAP %s
// RUN: %weavec_cc -fweavec-checks=trap -fweavec-print-prelude=inline -fsyntax-only %s -DNOT_COMPILED | FileCheck --check-prefix=TRAP %s
// RUN: %weavec_cc -fweavec-print-prelude -fweavec-checks=report | FileCheck --check-prefix=REPORT %s
// RUN: %weavec_cc -fweavec-print-prelude -fweavec-checks=verify | FileCheck --check-prefix=VERIFY %s
// RUN: %weavec_cc -fweavec-print-prelude -fweavec-checks=none | count 0
// RUN: %weavec_cc -fweavec-print-prelude -fno-weavec-zero-init | FileCheck --check-prefix=NOZERO %s
// RUN: %weavec_cc -fweavec-print-prelude --target=x86_64-unknown-linux-gnu | FileCheck --check-prefix=GLIBC %s
// RUN: %weavec_cc -fweavec-print-prelude -target aarch64-unknown-linux-android34 | FileCheck --check-prefix=BIONIC %s
// RUN: %weavec_cc -fweavec-print-prelude --target=arm64-apple-macosx14.0 | FileCheck --check-prefix=DARWIN %s
// RUN: %weavec_cc -fweavec-print-prelude --target=x86_64-pc-windows-msvc | FileCheck --check-prefix=NOQUERY %s
// RUN: %weavec_cc -fweavec-print-prelude=out-of-line -fweavec-checks=verify | FileCheck --check-prefix=OUTOFLINE %s
// RUN: rm -f %t.h && %weavec_cc -fweavec-print-prelude -o %t.h && FileCheck --check-prefix=TRAP %s < %t.h
//
// RUN: not %weavec_cc -fweavec-print-prelude=bogus 2>&1 | FileCheck --check-prefix=BOGUS %s
// RUN: not %weavec_cc -fweavec-print-prelude -fweavec-checks=abort 2>&1 | FileCheck --check-prefix=MODE %s
//
// TRAP: #pragma clang diagnostic ignored "-Weverything"
// TRAP: __builtin_verbose_trap("weavec", "nonnull");
// TRAP: __weavec_malloc_zero
// TRAP-NOT: __weavec_prv_
//
// REPORT: extern void __weavec_rt_report(const char *, const char *, unsigned, unsigned);
// REPORT: __weavec_rt_report("nonnull", file, line, column);
//
// VERIFY: __builtin_verbose_trap("weavec", "index");
// VERIFY: __weavec_prv_index
// VERIFY: __builtin_verbose_trap("weavec.proven", "index");
//
// NOZERO: __weavec_chk_nonnull
// NOZERO-NOT: __weavec_malloc_zero
//
// GLIBC: extern __typeof__(sizeof 0) malloc_usable_size(void *);
// BIONIC: extern __typeof__(sizeof 0) malloc_usable_size(const void *);
// DARWIN: extern __typeof__(sizeof 0) malloc_size(const void *);
//
// NOQUERY: __weavec_chk_nonnull
// NOQUERY-NOT: __weavec_malloc_zero
//
// OUTOFLINE: {{^}}void *__weavec_chk_nonnull(const volatile void *p) {
// OUTOFLINE: WEAVEC_CHK_TRAP("weavec.proven", "len")
// OUTOFLINE-NOT: static
//
// BOGUS: weavec-cc: error: invalid value 'bogus' in '-fweavec-print-prelude=bogus'; expected inline or out-of-line
// MODE: weavec-cc: error: invalid value 'abort' in '-fweavec-checks=abort'; expected trap, report, verify or none

#ifdef NOT_COMPILED
#error "-fweavec-print-prelude compiles nothing"
#endif

int main(void) { return 0; }
