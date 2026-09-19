// RFC 0030 §7.2: a WEAVEC_COUNTED_BY name that is not a sibling is an invalid annotation.
// STAGE: S6
// The argument must name a sibling parameter or field; 'count' names neither, so the warning
// is "'count' in WEAVEC_COUNTED_BY does not name a parameter or field" and the kind is
// dropped: p[i] keeps the A1 default and is unresolved(unknown-extent), never checked.
#include <stddef.h>
#include "../Inputs/rfc0030.h"

int at(const int *WEAVEC_COUNTED_BY(count) p, // BUG: invalid-annotation possible
       size_t n, size_t i) {
  (void)n;
  return p[i]; // UNRESOLVED: spatial:unknown-extent
}
