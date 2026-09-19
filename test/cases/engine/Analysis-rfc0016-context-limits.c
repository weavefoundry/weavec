// Engine pin converted from test/Analysis/rfc0016-context-limits.c; markers are the v0.10.0 golden diagnostics.
// RFC 0016: a missing contextual check is a coverage boundary.
#include "Inputs/prelude.h"

static void recurse(char *a, char *b, int n) {
  if (n) recurse(a, b, n - 1); // BUG: analysis-incomplete possible
  else { *b = 1; free(a); }
}
void deep(void) {
  char *p = malloc(4); if (!p) return;
  recurse(p, p, 20);
}
