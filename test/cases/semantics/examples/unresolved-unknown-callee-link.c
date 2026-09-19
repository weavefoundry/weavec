// RFC 0030 §4 "Unresolved, unknown callee", at link: the definition of 'consume' is seen.
// STAGE: S8
// Compiled alone, the unit leaves p[0] unresolved(unknown-callee). Linked by weavec-cc with
// a unit that defines 'consume' and frees 'p', the link step reports the use-after-free as
// an error and the link fails (§13.2).
// UNITS: Inputs/consume-frees.c
// ASAN
#include <stdlib.h>

void consume(char *p);

int f(void) {
  char *p = malloc(8);
  if (!p) return 0;
  consume(p);
  return p[0]; // BUG: use-after-free definite
}

int main(void) { return f(); }
