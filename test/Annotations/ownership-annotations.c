// Ownership annotations are accepted and --report-unannotated respects them.
// RUN: %weavec %s -- 2>&1 | count 0
// RUN: %weavec --report-unannotated %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

#if !WEAVEC_ENABLED
#error "weavec should define __WEAVEC__ when analysing"
#endif

struct buffer;

void annotated(struct buffer *WEAVEC_OWNED owned,
               const struct buffer *WEAVEC_BORROWED shared,
               struct buffer *WEAVEC_MUT exclusive) {
  use(owned);
  use((void *)shared);
  use(exclusive);
}

// CHECK: ownership-annotations.c:[[@LINE+1]]:23: warning: pointer parameter 'p' has no inferable ownership; annotate it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT [weavec::annotation-required]
void unannotated(int *p, int n) { use(p); }

// CHECK: 1 warning generated.
