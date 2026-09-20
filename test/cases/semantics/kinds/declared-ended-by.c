// RFC 0030 §7.2 (WEAVEC_ENDED_BY): [p, end) must lie in one object, which every call must meet.
// STAGE: S6
// 'p' is EndedBy(end). The first two calls pass ranges inside 'int a[4]'. The third passes
// [a, a + 5): the argument's exact extent is below the declared requirement for every
// value, so the call is a definite out-of-bounds error (§3.3).
#include "../Inputs/rfc0030.h"

int sum_range(const int *WEAVEC_ENDED_BY(end) p, const int *end) {
  int s = 0;
  for (; p < end; p++) s += *p;
  return s;
}

int total(void) {
  int a[4] = {1, 2, 3, 4};
  int s = sum_range(a, a) + sum_range(a, a + 4);
  return s + sum_range(a, a + 5); // BUG: out-of-bounds definite
}
