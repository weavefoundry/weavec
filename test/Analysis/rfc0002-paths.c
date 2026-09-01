// RFC 0002: the checker is a dataflow over the CFG, so back edges, switch
// fallthrough, goto and short-circuit operands are all real paths.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

void loop(int n) {
  char *p = malloc(8);
  for (int i = 0; i < n; ++i) {
    // CHECK: rfc0002-paths.c:[[@LINE+1]]:5: error: use of 'p' after it was freed [weavec::use-after-free]
    p[0] = 0;
    // CHECK: rfc0002-paths.c:[[@LINE+3]]:5: note: freed here
    // CHECK: rfc0002-paths.c:[[@LINE+2]]:5: error: 'p' is freed twice [weavec::double-free]
    // CHECK: rfc0002-paths.c:[[@LINE+1]]:5: note: previously freed here
    free(p);
  }
}

void fallthrough(int c) {
  char *p = malloc(8);
  switch (c) {
  case 0: free(p);
  // CHECK: rfc0002-paths.c:[[@LINE+1]]:11: error: 'p' is freed twice [weavec::double-free]
  case 1: free(p);
  // CHECK: rfc0002-paths.c:[[@LINE-3]]:11: note: previously freed here
  }
}

void backwards_goto(int n) {
  char *p = malloc(4);
again:
  // CHECK: rfc0002-paths.c:[[@LINE+1]]:3: error: 'p' is freed twice [weavec::double-free]
  free(p);
  if (n--)
    goto again;
}

void short_circuit(int c) {
  char *p = malloc(4);
  if (c && (free(p), 1)) {
  }
  // CHECK: rfc0002-paths.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

void do_while(int n) {
  char *p = malloc(4);
  do {
    // CHECK: rfc0002-paths.c:[[@LINE+1]]:9: error: use of 'p' after it was freed [weavec::use-after-free]
    use(p);
    // CHECK: rfc0002-paths.c:[[@LINE+1]]:5: error: 'p' is freed twice [weavec::double-free]
    free(p);
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

// CHECK: 7 errors generated.
