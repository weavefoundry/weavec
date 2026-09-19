// Engine pin converted from test/Analysis/branches.c; markers are the v0.10.0 golden diagnostics.
// Paths are joined conservatively: a pointer freed on any path is treated as
// possibly freed afterwards.
#include "Inputs/prelude.h"

void maybe_freed(int c) {
  int *p = malloc(4);
  if (c)
    free(p);
  else
    use(p); // fine: p is live on this path
  use(p); // BUG: use-after-free definite
}

void freed_in_loop(int n) {
  int *p = malloc(4);
  for (int i = 0; i < n; ++i)
    free(p); // BUG: double-free definite
  use(p); // BUG: use-after-free definite
}
