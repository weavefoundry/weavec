// RFC 0008, *Diagnostics*: `null-dereference`, `use-of-uninitialized` and
// `invalid-release` are errors by default; each can be lowered to a warning
// but not disabled.
// RUN: not %weavec %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: %weavec -Wno-error=weavec-null-dereference -Wno-error=weavec-use-of-uninitialized -Wno-error=weavec-invalid-release %s -- 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: not %weavec -Wno-weavec-null-dereference %s -- 2>&1 | FileCheck --check-prefix=REFUSED-NULL %s
// RUN: not %weavec -Wno-weavec-use-of-uninitialized %s -- 2>&1 | FileCheck --check-prefix=REFUSED-UNINIT %s
// RUN: not %weavec -Wno-weavec-invalid-release %s -- 2>&1 | FileCheck --check-prefix=REFUSED-RELEASE %s
#include <stdlib.h>

int null_deref(void) {
  int *p = malloc(sizeof *p);
  // DEFAULT: rfc0008-flags.c:[[@LINE+2]]:12: error: dereference of 'p', which may be null [weavec::null-dereference]
  // LOWERED: rfc0008-flags.c:[[@LINE+1]]:12: warning: dereference of 'p', which may be null [weavec::null-dereference]
  int v = *p;
  free(p);
  return v;
}

void uninit(void) {
  char *p;
  // DEFAULT: rfc0008-flags.c:[[@LINE+2]]:3: error: use of 'p' before it was initialized [weavec::use-of-uninitialized]
  // LOWERED: rfc0008-flags.c:[[@LINE+1]]:3: warning: use of 'p' before it was initialized [weavec::use-of-uninitialized]
  free(p);
}

void release(void) {
  char buf[8];
  // DEFAULT: rfc0008-flags.c:[[@LINE+2]]:3: error: 'buf' is released but is not a heap object [weavec::invalid-release]
  // LOWERED: rfc0008-flags.c:[[@LINE+1]]:3: warning: 'buf' is released but is not a heap object [weavec::invalid-release]
  free(buf);
}

// REFUSED-NULL: error: '-Wno-weavec-null-dereference': 'null-dereference' is an error and cannot be disabled; use -Wno-error=weavec-null-dereference to make it a warning
// REFUSED-UNINIT: error: '-Wno-weavec-use-of-uninitialized': 'use-of-uninitialized' is an error and cannot be disabled; use -Wno-error=weavec-use-of-uninitialized to make it a warning
// REFUSED-RELEASE: error: '-Wno-weavec-invalid-release': 'invalid-release' is an error and cannot be disabled; use -Wno-error=weavec-invalid-release to make it a warning
