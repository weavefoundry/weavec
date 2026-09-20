// RFC 0030 §3.1 and §2.6: 'two(p, p)' frees through one parameter and writes through the other.
// STAGE: S3
// Inside 'two', 'b' is a parameter the analysis cannot prove distinct from the released 'a'
// (both point to char), so the authoritative row of b[0] is unresolved(may-alias-released)
// with no diagnostic of its own. The call two(p, p) is still reported through the context
// run, as v0.10.0 reports it (the Departure of §2.6): a definite use-after-free at the use
// in the callee, linked to the call, whose temporal facet is a violation.
// ASAN
#include <stdlib.h>

void two(char *a, char *b) {
  free(a);
  b[0] = 1; // BUG: use-after-free definite // UNRESOLVED: temporal:may-alias-released
}

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  two(p, p); // NOT-PROVEN: temporal
  return 0;
}
