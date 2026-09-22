// RFC 0005, *Flags*: compiler-style control of WeaveC's diagnostics, on
// both tools, and the driver's -fweavec-* switches.
//
// A warning can be disabled and re-enabled; an error can only be lowered.
// RFC 0030: -Wno-weavec-<id> drops the warnings of an id whose possible
// findings are warnings, and leaves its definite errors; an id RFC 0030
// removed is unknown.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: not %weavec %s -- -DBUG 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: %weavec -Wno-weavec-leak %s -- 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// RUN: %weavec -Wno-weavec-leak -Wweavec-leak %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: %weavec -Wno-weavec %s -- 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// RUN: not %weavec -Werror=weavec-leak %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: not %weavec -Werror=weavec %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: %weavec -Wno-error=weavec-use-after-free %s -- -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: %weavec -Wno-error=weavec %s -- -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: not %weavec -Wno-weavec-use-after-free %s -- -DBUG 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: not %weavec -Wno-weavec-unsafe-operation %s -- 2>&1 | FileCheck --check-prefix=REFUSED %s
// RUN: not %weavec -Wno-weavec-nonsense %s -- 2>&1 | FileCheck --check-prefix=UNKNOWN %s
// RUN: not %weavec -Wno-weavec-annotation-required %s -- 2>&1 | FileCheck --check-prefix=REMOVED %s
//
// The same spellings on the driver. -fno-weavec compiles without analysis
// and without a unit record; -fsyntax-only analyses but writes nothing.
// RUN: rm -rf %t && mkdir -p %t
// RUN: not %weavec_cc -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: not ls %t/bug.o
// RUN: not ls %t/bug.o.weavec
// RUN: %weavec_cc -Wno-error=weavec -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: %weavec --dump-record=%t/bug.o.weavec | FileCheck --check-prefix=RECORDED %s
// RUN: %weavec_cc -fno-weavec -c %s -o %t/plain.o -DBUG 2>&1 | count 0
// RUN: not ls %t/plain.o.weavec
// RUN: %weavec_cc -fsyntax-only -Wno-weavec-leak %s 2>&1 | count 0
// RUN: not %weavec_cc -fweavec-bogus -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=BOGUS %s
// RUN: not %weavec_cc -Wno-weavec-unsafe-operation -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=REFUSED %s
//
// WeaveC's flags reach every cc1 job (as -Xclang arguments) and Clang's
// driver never sees them as its own.
// RUN: %weavec_cc -### -fno-weavec-zero-init -Wno-weavec-leak -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=JOBS %s
// RUN: %weavec_cc --version | FileCheck --check-prefix=VERSION %s
#include "../Inputs/prelude.h"

// DEFAULT: warning: 'q' is leaked [weavec::leak]
// DEFAULT-NOT: error:
// RAISED: error: 'q' is leaked [weavec::leak]
// REFUSED: error: '-Wno-weavec-unsafe-operation': 'unsafe-operation' is an error and cannot be disabled; use -Wno-error=weavec-unsafe-operation to make it a warning
// QUIET-NOT: {{warning|error}}:
// UNKNOWN: error: unknown WeaveC diagnostic 'nonsense' in '-Wno-weavec-nonsense'
// REMOVED: error: unknown WeaveC diagnostic 'annotation-required' (removed by RFC 0030)
// BOGUS: weavec-cc: error: unknown WeaveC flag '-fweavec-bogus'
// JOBS: "-cc1"
// JOBS-SAME: "-D" "__WEAVEC__=1"
// JOBS-SAME: "-fno-weavec-zero-init" "-Wno-weavec-leak"
// JOBS-NOT: error:
// VERSION: weavec-cc version {{[0-9]+\.[0-9]+\.[0-9]+}}
// VERSION-NEXT: built with LLVM {{[0-9]+\.}}
// RECORDED: "reported": [
// RECORDED: "id": "use-after-free",
// RECORDED-NEXT: "file": "{{.*}}rfc0005-flags.c",
// RECORDED-NEXT: "line": [[#]],
// RECORDED-NEXT: "column": 10
void f(void) {
  char *q = malloc(1);
  (void)q;
}

#ifdef BUG
int g(void) {
  char *p = malloc(1);
  free(p);
  // BUG: rfc0005-flags.c:[[@LINE+2]]:10: error: use of 'p' after it was freed [weavec::use-after-free]
  // LOWERED: rfc0005-flags.c:[[@LINE+1]]:10: warning: use of 'p' after it was freed [weavec::use-after-free]
  return p[0];
}
#endif
