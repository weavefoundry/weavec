// RFC 0030 §8, §5.3: what the library table says beyond a summary. Hidden
// state (`retain`, `reads`, `invalidates`), a `sync` callback's target as a
// may-effect of the call, `alloca` storage, and a literal format's arity.
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *next_field(void) {
  char *s = strdup("a,b");
  if (!s)
    return NULL;
  strtok(s, ",");
  free(s);
  // CHECK: rfc0030-library.c:[[@LINE+1]]:10: error: use of '<strtok>' after it was freed [weavec::use-after-free]
  return strtok(NULL, ",");
}

int stale_environment(void) {
  const char *v = getenv("WEAVEC_LIT");
  setenv("WEAVEC_LIT", "other", 1);
  // CHECK: rfc0030-library.c:[[@LINE+1]]:14: warning: use of 'v' after it may have been freed [weavec::use-after-free]
  return v ? v[0] : 0;
}

static char *victim;
static int compare(const void *a, const void *b) {
  free(victim);
  victim = NULL;
  return *(const int *)a - *(const int *)b;
}

int sort_then_use(char *p) {
  int xs[2] = {2, 1};
  victim = p;
  qsort(xs, 2, sizeof xs[0], compare);
  // CHECK: rfc0030-library.c:[[@LINE+1]]:10: warning: use of 'p' after it may have been freed [weavec::use-after-free]
  return p[0];
}

int sort_unknown(int (*order)(const void *, const void *)) {
  int xs[2] = {2, 1};
  qsort(xs, 2, sizeof xs[0], order);
  return xs[0];
}
// LEDGER: "reason": "callback",
// LEDGER-NEXT: "detail": "the target of the callback of 'qsort' is unknown"

char *frame(void) {
  char *p = alloca(8);
  p[0] = 1;
  // CHECK: rfc0030-library.c:[[@LINE+1]]:10: error: 'p' may outlive '<alloca>', which it points to [weavec::lifetime-too-short]
  return p;
}

void too_few(const char *s) {
  // CHECK: rfc0030-library.c:[[@LINE+1]]:3: error: format string of 'printf' reads 2 arguments but 1 are passed [weavec::out-of-bounds]
  printf("%s %s\n", s);
}
