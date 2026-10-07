// RFC 0030 §5.5, RFC 0034 §7.1: a function whose analysis would take more
// work (the sizes of the states it transfers and joins) than its budget
// stops; its operations take the §2.6 defaults with reason `budget`, the
// summary line lists it, and its callers apply the unknown-callee default
// at its calls.
// RUN: %weavec --budget=100 %s -- 2>&1 | FileCheck %s
// RUN: %weavec --budget=0 %s -- 2>&1 | FileCheck --check-prefix=FULL %s
#include <stdlib.h>

// CHECK: rfc0030-budget.c: {{.*}}; 1 function over budget (walk)
// FULL-NOT: over budget
int walk(const int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < i; j++)
      if (p[i] > p[j])
        s += p[j];
  return s;
}

// The caller stays within its budget; the call to `walk` may have done
// anything to what it was handed.
int caller(void) {
  int *p = malloc(4 * sizeof *p);
  if (!p)
    return 0;
  p[0] = 1;
  // The call's own facet is about `p`, which is live here: RFC 0031 §5.4
  // decides it from the caller's state, so it is proven; what `walk` did is
  // the unknown-callee default after the call.
  int s = walk(p, 4);
  s += p[0];
  free(p);
  return s;
}
