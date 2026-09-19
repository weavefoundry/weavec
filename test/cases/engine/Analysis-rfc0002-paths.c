// Engine pin converted from test/Analysis/rfc0002-paths.c; markers are the v0.10.0 golden diagnostics.
// RFC 0002: the checker is a dataflow over the CFG, so back edges, switch
// fallthrough, goto and short-circuit operands are all real paths.
#include "Inputs/prelude.h"

void loop(int n) {
  char *p = malloc(8);
  for (int i = 0; i < n; ++i) {
    p[0] = 0; // BUG: use-after-free definite
    free(p); // BUG: double-free definite
  }
}

void fallthrough(int c) {
  char *p = malloc(8);
  switch (c) { // BUG: leak possible
  case 0: free(p);
  case 1: free(p); // BUG: double-free definite
  }
}

void backwards_goto(int n) {
  char *p = malloc(4);
again:
  free(p); // BUG: double-free definite
  if (n--)
    goto again;
}

void short_circuit(int c) {
  char *p = malloc(4);
  if (c && (free(p), 1)) {
  }
  use(p); // BUG: use-after-free definite
}

void do_while(int n) {
  char *p = malloc(4);
  do {
    use(p); // BUG: use-after-free definite
    free(p); // BUG: double-free definite
  } while (n--);
}

// Freeing on every path is not a double free, and reinitialisation kills
// the fact on the paths where it happens.
void clean(int c) {
  char *p = malloc(4);
  if (c)
    free(p);
  else
    free(p);
  p = malloc(4);
  if (!p)
    return;
  free(p);
}
