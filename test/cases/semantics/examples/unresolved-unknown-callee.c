// RFC 0030 §4 "Unresolved, unknown callee": a use after an unknown callee is not proven.
// STAGE: S3
// 'consume' has no body, summary, table entry or ownership contract, so it may have freed
// 'p' (§5.1). Its Call site and the later p[0] have temporal unresolved(unknown-callee), and
// there is no diagnostic. The link half of the example is unresolved-unknown-callee-link.c.
// CLEAN
#include <stdlib.h>

void consume(char *p);

int f(void) {
  char *p = malloc(8);
  if (!p) return 0;
  consume(p); // UNRESOLVED: temporal:unknown-callee
  return p[0]; // UNRESOLVED: temporal:unknown-callee
}
