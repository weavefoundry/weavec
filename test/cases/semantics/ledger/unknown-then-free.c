// RFC 0030 §5.1 and §3.1: a known release after an unknown callee replaces the unknown record.
// STAGE: S3
// 'consume' is an unknown callee, so it marks 'p' Freed with unknownOrigin. The free after
// it hits that record: its own temporal facet is unresolved(unknown-callee) without a
// diagnostic (the callee may have released 'p' already), and the record is replaced by a
// known one, so the write after the free is still the definite use-after-free v0.10.0
// reports.
#include <stdlib.h>

void consume(char *p);

void f(void) {
  char *p = malloc(8);
  if (!p) return;
  consume(p); // UNRESOLVED: temporal:unknown-callee
  free(p); // UNRESOLVED: temporal:unknown-callee
  p[0] = 1; // BUG: use-after-free definite
}
