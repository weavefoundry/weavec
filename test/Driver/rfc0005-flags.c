// RFC 0005, *Flags*: compiler-style control of WeaveC's diagnostics, on
// both tools, and the driver's -fweavec-* switches.
//
// A warning can be disabled and re-enabled; an error can only be lowered.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: not %weavec %s -- -DBUG 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: %weavec -Wno-weavec-annotation-required %s -- 2>&1 | count 0
// RUN: %weavec -Wno-weavec-annotation-required -Wweavec-annotation-required %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: %weavec -Wno-weavec %s -- 2>&1 | count 0
// RUN: not %weavec -Werror=weavec-annotation-required %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: not %weavec -Werror=weavec %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: %weavec -Wno-error=weavec-use-after-free %s -- -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: %weavec -Wno-error=weavec %s -- -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: not %weavec -Wno-weavec-use-after-free %s -- 2>&1 | FileCheck --check-prefix=REFUSED %s
// RUN: not %weavec -Wno-weavec-nonsense %s -- 2>&1 | FileCheck --check-prefix=UNKNOWN %s
//
// The same spellings on the driver. -fno-weavec compiles without analysis
// and without a sidecar; -fsyntax-only analyses but writes nothing.
// RUN: rm -rf %t && mkdir -p %t
// RUN: not %weavec_cc -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: not ls %t/bug.o
// RUN: not ls %t/bug.o.weavec
// RUN: %weavec_cc -Wno-error=weavec -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: FileCheck --check-prefix=RECORDED %s < %t/bug.o.weavec
// RUN: %weavec_cc -fno-weavec -c %s -o %t/plain.o -DBUG 2>&1 | count 0
// RUN: not ls %t/plain.o.weavec
// RUN: %weavec_cc -fsyntax-only %s 2>&1 | count 0
// RUN: not %weavec_cc -fweavec-strict -c %s -o %t/strict.o 2>&1 | FileCheck --check-prefix=STRICT %s
// RUN: not %weavec_cc -fweavec-bogus -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=BOGUS %s
// RUN: not %weavec_cc -Wno-weavec-use-after-free -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=REFUSED %s
//
// WeaveC's flags reach every cc1 job (as -Xclang arguments) and Clang's
// driver never sees them as its own.
// RUN: %weavec_cc -### -fweavec-strict -Wno-weavec-annotation-required -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=JOBS %s
// RUN: %weavec_cc --version | FileCheck --check-prefix=VERSION %s
#include "../Inputs/prelude.h"

void mystery(void *p);

// DEFAULT: warning: call to 'mystery' is not checked{{.*}}[weavec::annotation-required]
// DEFAULT-NOT: error:
// RAISED: error: call to 'mystery' is not checked{{.*}}[weavec::annotation-required]
// STRICT: error: unchecked call to 'mystery' outside an unsafe region [weavec::unsafe-operation]
// REFUSED: error: '-Wno-weavec-use-after-free': 'use-after-free' is an error and cannot be disabled; use -Wno-error=weavec-use-after-free to make it a warning
// UNKNOWN: error: unknown WeaveC diagnostic 'nonsense' in '-Wno-weavec-nonsense'
// BOGUS: weavec-cc: error: unknown WeaveC flag '-fweavec-bogus'
// JOBS: "-cc1"
// JOBS-SAME: "-D" "__WEAVEC__=1"
// JOBS-SAME: "-fweavec-strict" "-Wno-weavec-annotation-required"
// JOBS-NOT: error:
// VERSION: weavec-cc version {{[0-9]+\.[0-9]+\.[0-9]+}}
// VERSION-NEXT: built with LLVM {{[0-9]+\.}}
// RECORDED: reported use-after-free [[#]] 10 {{.*}}rfc0005-flags.c
void f(void *p) { mystery(p); }

#ifdef BUG
int g(void) {
  char *p = malloc(1);
  free(p);
  // BUG: rfc0005-flags.c:[[@LINE+2]]:10: error: use of 'p' after it was freed [weavec::use-after-free]
  // LOWERED: rfc0005-flags.c:[[@LINE+1]]:10: warning: use of 'p' after it was freed [weavec::use-after-free]
  return p[0];
}
#endif
