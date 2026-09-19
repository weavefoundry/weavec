// Ownership annotations are accepted, and unannotated exported functions
// are not reported. RFC 0030 §16 removes --report-unannotated, which
// offered the inferred annotations as fix-its; the ledger carries them.
// RUN: %weavec %s -- 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// QUIET-NOT: {{warning|error}}:
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

void reads(int *p, int n) { use(p); }

void frees(struct buffer *p) { free(p); }

void writes(int *out, int v) { *out = v; }

void *makes(void) { return malloc(8); }

void untouched(int *p) {}

// Static helpers are not part of the exported surface.
static void helper(int *p) { free(p); }
void caller(void) { helper(malloc(4)); }

