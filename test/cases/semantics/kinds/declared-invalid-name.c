// RFC 0030 §7.2: a WEAVEC_COUNTED_BY name that is not a sibling is an invalid annotation.
// STAGE: S6
// The argument must name a sibling parameter or field; 'count' names neither, so the warning
// is "'count' in WEAVEC_COUNTED_BY does not name a parameter or field" and the kind is
// dropped: p keeps the A1 default, and p[i] is never checked. It is not unresolved either:
// the unguarded p[i] is a must-access that gives Counted(i + 1) (§7.5 R5), and 'at' is
// exported, so the access that requirement covers is trusted(caller-contract).
#include <stddef.h>
#include "../Inputs/rfc0030.h"

int at(const int *WEAVEC_COUNTED_BY(count) p, // BUG: invalid-annotation possible
       size_t n, size_t i) {
  (void)n;
  return p[i]; // TRUSTED: spatial:caller-contract
}
