// RUN: %weavec --checked-function=same_array %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=snapshot %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=live_allocation %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=unrelated %s -- 2>&1 | FileCheck %s --check-prefix=UNRELATED
// RUN: not %weavec --checked-function=subobjects %s -- 2>&1 | FileCheck %s --check-prefix=UNRELATED
// RUN: not %weavec --checked-function=unbounded_difference %s -- 2>&1 | FileCheck %s --check-prefix=DIFFERENCE
// RUN: not %weavec --checked-function=released -Wno-error=weavec %s -- -DRELEASED 2>&1 | FileCheck %s --check-prefix=RELEASED
// RFC 0021: provenance, live storage and target ptrdiff_t representability
// are independent premises. One-past formation permits no dereference.

#include <stddef.h>
#include <stdlib.h>

ptrdiff_t same_array(void) {
  int a[4];
  int *end = a + 4;
  if (end > a)
    return end - a;
  return a - end;
}

ptrdiff_t snapshot(void) {
  char a[4];
  unsigned n = 4;
  char *end = a + n;
  n = 8;
  return end - a;
}

ptrdiff_t live_allocation(void) {
  char *p = malloc(4);
  if (!p)
    return 0;
  char *end = p + 4;
  ptrdiff_t size = end - p;
  free(p);
  return size;
}

ptrdiff_t unrelated(void) {
  int a[4], b[4];
  return a - b;
}

ptrdiff_t subobjects(void) {
  struct { int a[4]; int b[4]; } s;
  return s.a - s.b;
}

ptrdiff_t unbounded_difference(char *p, size_t n) {
  char *end = p + n;
  return end - p;
}

#ifdef RELEASED
ptrdiff_t released(void) {
  char *p = malloc(4);
  if (!p)
    return 0;
  char *end = p + 4;
  free(p);
  return end - p;
}
#endif

// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// UNRELATED: error: cannot establish checked safety: pointer difference or ordering needs shared-object evidence [weavec::checking-incomplete]
// UNRELATED: error: checked safety requirements were not established
// RELEASED: warning: checked safety failed: use of 'end' after it was freed [weavec::checking-failed]
// RELEASED: error: checked safety requirements were not established
// DIFFERENCE: error: cannot establish checked safety: pointer difference must fit target ptrdiff_t and element size [weavec::checking-incomplete]
// DIFFERENCE: error: checked safety requirements were not established
