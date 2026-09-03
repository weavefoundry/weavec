// Ownership annotations are accepted and --report-unannotated offers the
// inferred annotation for exported functions as a fix-it (RFC 0003).
// RUN: %weavec %s -- 2>&1 | count 0
// RUN: %weavec --report-unannotated %s -- 2>&1 | FileCheck %s
// RUN: %weavec --report-unannotated %s -- -fdiagnostics-parseable-fixits 2>&1 | FileCheck --check-prefix=FIXIT %s
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
  free(owned); /* an owned parameter must be released (RFC 0007) */
}

// CHECK: ownership-annotations.c:[[@LINE+2]]:17: warning: pointer parameter 'p' of 'reads' is inferred WEAVEC_BORROWED; add the annotation to its declaration [weavec::annotation-required]
// FIXIT: fix-it:{{.*}}ownership-annotations.c":{[[@LINE+1]]:17-[[@LINE+1]]:17}:"WEAVEC_BORROWED "
void reads(int *p, int n) { use(p); }

// CHECK: ownership-annotations.c:[[@LINE+1]]:27: warning: pointer parameter 'p' of 'frees' is inferred WEAVEC_OWNED; add the annotation to its declaration [weavec::annotation-required]
void frees(struct buffer *p) { free(p); }

// CHECK: ownership-annotations.c:[[@LINE+1]]:18: warning: pointer parameter 'out' of 'writes' is inferred WEAVEC_MUT; add the annotation to its declaration [weavec::annotation-required]
void writes(int *out, int v) { *out = v; }

// CHECK: ownership-annotations.c:[[@LINE+1]]:7: warning: return value of 'makes' is inferred WEAVEC_OWNED; add the annotation to its declaration [weavec::annotation-required]
void *makes(void) { return malloc(8); }

// CHECK: ownership-annotations.c:[[@LINE+1]]:21: warning: pointer parameter 'p' has no inferable ownership; annotate it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT [weavec::annotation-required]
void untouched(int *p) {}

// Static helpers are not part of the exported surface.
static void helper(int *p) { free(p); }
void caller(void) { helper(malloc(4)); }

// CHECK: 5 warnings generated.
