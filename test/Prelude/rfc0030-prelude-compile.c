/* RFC 0030, section 10.2: the check prelude of every mode compiles, warning
 * free, under -std=c89 -pedantic-errors -Weverything -Werror and every later
 * standard, both included into a unit and as preprocessed (.i) input, where
 * no predefined macro is expanded. This file is the unit: C89, block
 * comments only.
 *
 * RUN: rm -rf %t && mkdir -p %t
 * RUN: %weavec_prelude --mode=trap -o %t/trap.h
 * RUN: %weavec_prelude --mode=report -o %t/report.h
 * RUN: %weavec_prelude --mode=verify -o %t/verify.h
 * RUN: %weavec_prelude --mode=none -o %t/none.h
 * RUN: %weavec_prelude --mode=trap --no-zero-init --no-verbose-trap -o %t/plain.h
 *
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/trap.h %s
 * RUN: %clang -std=c99 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/trap.h %s
 * RUN: %clang -std=c11 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/trap.h %s
 * RUN: %clang -std=c17 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/trap.h %s
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/trap.h %s
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/report.h %s
 * RUN: %clang -std=c99 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/report.h %s
 * RUN: %clang -std=c11 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/report.h %s
 * RUN: %clang -std=c17 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/report.h %s
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/report.h %s
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/verify.h %s
 * RUN: %clang -std=c99 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/verify.h %s
 * RUN: %clang -std=c11 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/verify.h %s
 * RUN: %clang -std=c17 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/verify.h %s
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/verify.h %s
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/none.h %s
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/none.h %s
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/plain.h %s
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -fsyntax-only -include %t/plain.h %s
 *
 * Code generation, at -O0 and -O2, for the largest prelude.
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -O0 -c -include %t/verify.h %s -o %t/verify-O0.o
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -O2 -c -include %t/report.h %s -o %t/report-O2.o
 *
 * Preprocessed input: the prelude and this unit in one .i file.
 * RUN: cat %t/trap.h %s > %t/trap.i
 * RUN: cat %t/report.h %s > %t/report.i
 * RUN: cat %t/verify.h %s > %t/verify.i
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -c %t/trap.i -o %t/trap.o
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -c %t/trap.i -o %t/trap.o
 * RUN: %clang -std=c89 -pedantic-errors -Weverything -Werror -c %t/report.i -o %t/report.o
 * RUN: %clang -std=c2x -pedantic-errors -Weverything -Werror -c %t/verify.i -o %t/verify.o
 *
 * What each mode defines.
 * RUN: FileCheck --check-prefix=TRAP %s < %t/trap.h
 * RUN: FileCheck --check-prefix=REPORT %s < %t/report.h
 * RUN: FileCheck --check-prefix=VERIFY %s < %t/verify.h
 * RUN: FileCheck --check-prefix=PLAIN %s < %t/plain.h
 * RUN: count 0 < %t/none.h
 *
 * TRAP: #pragma clang diagnostic ignored "-Weverything"
 * TRAP: static __inline__ __attribute__((always_inline, nodebug, unused)) void *__weavec_chk_nonnull(const volatile void *p) {
 * TRAP: __builtin_verbose_trap("weavec", "nonnull");
 * TRAP: __weavec_chk_violation(void)
 * TRAP: __builtin_verbose_trap("weavec", "violation");
 * TRAP: __weavec_need_add
 * TRAP: __weavec_malloc_zero
 * TRAP: #pragma clang diagnostic pop
 *
 * REPORT: extern void __weavec_rt_report(const char *, const char *, unsigned, unsigned);
 * REPORT: __weavec_chk_nonnull(const volatile void *p, const char *file, unsigned line, unsigned column)
 * REPORT: __weavec_rt_report("nonnull", file, line, column);
 * REPORT: __weavec_chk_violation(const char *file, unsigned line, unsigned column)
 * REPORT-NOT: __builtin_verbose_trap
 *
 * VERIFY: __weavec_chk_index
 * VERIFY: __builtin_verbose_trap("weavec", "index");
 * VERIFY: __weavec_prv_index
 * VERIFY: __builtin_verbose_trap("weavec.proven", "index");
 *
 * PLAIN: __builtin_trap();
 * PLAIN-NOT: __builtin_verbose_trap
 * PLAIN-NOT: __weavec_malloc_zero
 */

int main(void) { return 0; }
