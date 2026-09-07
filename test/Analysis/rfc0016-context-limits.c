// RFC 0016: a missing contextual check is a coverage boundary.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

static void recurse(char *a, char *b, int n) {
  // CHECK: rfc0016-context-limits.c:[[@LINE+1]]:10: warning: analysis is incomplete: call context unavailable or limit reached [weavec::analysis-incomplete]
  if (n) recurse(a, b, n - 1);
  else { *b = 1; free(a); }
}
void deep(void) {
  char *p = malloc(4); if (!p) return;
  recurse(p, p, 20);
}
