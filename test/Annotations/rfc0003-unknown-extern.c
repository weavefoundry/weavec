// RFC 0003: a call to a function with neither a definition in this
// translation unit nor ownership annotations is a checking boundary. It warns
// once per callee by default and is silent for system headers (which the
// shipped libc table covers). Under --strict-externs it is a raw operation
// (RFC 0004, *Boundaries*): an error at every call outside an unsafe region.
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
  // CHECK: note: annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this translation unit
  // STRICT: rfc0003-unknown-extern.c:[[@LINE-3]]:3: error: unchecked call to 'mystery' outside an unsafe region [weavec::unsafe-operation]
  // STRICT: rfc0003-unknown-extern.c:[[@LINE-11]]:6: note: 'mystery' is declared here
  // STRICT: note: annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, define it in this translation unit, or move the call into a WEAVEC_UNSAFE region
  // STRICT: rfc0003-unknown-extern.c:[[@LINE+1]]:3: error: unchecked call to 'mystery' outside an unsafe region [weavec::unsafe-operation]
  mystery(p);
  pure(1);
  annotated(p);
  // System headers are exempt from the default warning (the libc table
  // covers them) but not from strict mode: unchecked is unchecked.
  // STRICT: rfc0003-unknown-extern.c:[[@LINE+1]]:3: error: unchecked call to 'vendor_touch' outside an unsafe region [weavec::unsafe-operation]
  vendor_touch(p);
  // CHECK: rfc0003-unknown-extern.c:[[@LINE+4]]:7: warning: call to 'maker' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  // STRICT: rfc0003-unknown-extern.c:[[@LINE+3]]:7: error: 'use' dereferences raw pointer outside an unsafe region [weavec::unsafe-operation]
  // STRICT: note: the pointer is raw: returned by a call into unchecked code ('maker') here
  // STRICT: rfc0003-unknown-extern.c:[[@LINE+1]]:7: error: unchecked call to 'maker' outside an unsafe region [weavec::unsafe-operation]
  use(maker());
}

// The region's author vouches for the callee: nothing is reported inside.
void vouched(char *p) {
  WEAVEC_UNSAFE { mystery(p); }
}

// CHECK: 2 warnings generated.
// STRICT: 5 errors generated.
