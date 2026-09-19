// RFC 0030 §5.7: an asm statement applies the unknown-callee default to its pointer operands.
// STAGE: S3
// The asm statement takes 'p' as an operand, so it may have released, retained or replaced
// the object (detail "inline assembly"): the later read and the release are
// unresolved(unknown-callee), with no diagnostic.
// CLEAN
#include <stdlib.h>

int f(void) {
  char *p = malloc(8);
  if (!p) return 0;
  p[0] = 1;
  __asm__ volatile("" : : "r"(p) : "memory");
  int v = p[0]; // UNRESOLVED: temporal:unknown-callee
  free(p); // UNRESOLVED: temporal:unknown-callee
  return v;
}
