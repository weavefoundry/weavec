// RFC 0003: a call to a function with neither a definition in this
// translation unit nor ownership annotations is a checking boundary. It warns
// once per callee by default, is an error under --strict-externs, and is
// silent for system headers (which the shipped libc table covers).
// RUN: %weavec %s -- -isystem %S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --strict-externs %s -- -isystem %S/Inputs 2>&1 | FileCheck --check-prefix=STRICT %s
#include "../Inputs/prelude.h"
#include <weavec.h>
#include <vendor.h>

void mystery(void *p);
int pure(int x);
void *maker(void);
void annotated(void *WEAVEC_BORROWED p);

void f(char *p) {
  // CHECK: rfc0003-unknown-extern.c:[[@LINE+1]]:3: warning: call to 'mystery' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  mystery(p);
  // CHECK: rfc0003-unknown-extern.c:[[@LINE-8]]:6: note: 'mystery' is declared here
  // CHECK: note: annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT, or define it in this translation unit
  mystery(p);
  pure(1);
  annotated(p);
  vendor_touch(p);
  // CHECK: rfc0003-unknown-extern.c:[[@LINE+1]]:7: warning: call to 'maker' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  use(maker());
}

// CHECK: 2 warnings generated.

// STRICT: rfc0003-unknown-extern.c:{{[0-9]+}}:3: error: call to 'mystery' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
// STRICT: rfc0003-unknown-extern.c:{{[0-9]+}}:7: error: call to 'maker' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
// STRICT: 2 errors generated.
