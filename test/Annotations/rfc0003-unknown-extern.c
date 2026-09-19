// RFC 0003: a call to a function with neither a definition in this
// translation unit nor ownership annotations is a checking boundary. It warns
// once per callee by default and is silent for system headers (which the
// shipped libc table covers). RFC 0030 §16 removes --strict-externs, which
// made it an error at every call outside an unsafe region.
// RUN: %weavec %s -- -isystem %S/Inputs 2>&1 | FileCheck %s
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
  // CHECK: note: annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this program
  mystery(p);
  pure(1);
  annotated(p);
  // System headers are exempt from the default warning (the libc table
  // covers them).
  vendor_touch(p);
  // CHECK: rfc0003-unknown-extern.c:[[@LINE+1]]:7: warning: call to 'maker' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  use(maker());
}

// The region's author vouches for the callee: nothing is reported inside.
void vouched(char *p) {
  WEAVEC_UNSAFE { mystery(p); }
}

// CHECK: 2 warnings generated.
