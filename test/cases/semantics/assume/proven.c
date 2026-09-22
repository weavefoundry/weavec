// RFC 0030 §6.2: an assumption the engine proves stays weavec.h's no-op call and needs no check.
// STAGE: S3
// After the early return 'n > 0' holds at the Assume site, so its assertion facet is proven
// and the call stays the no-op weavec_assume_, which the optimiser removes.
// CLEAN
// EXPECT-LEDGER: /summary/facets/assertion/proven == 1
// EXPECT-LEDGER: /summary/facets/assertion/checked == 0
#include <weavec.h>

int positive(int n) {
  if (n <= 0) return 0;
  WEAVEC_ASSUME(n > 0);
  return n;
}
