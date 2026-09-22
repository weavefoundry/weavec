// RFC 0016: callers with related pointers retain safe ordering and replacement.
// The contexts are clean. RFC 0030 §2.6: every function also gets the generic
// (authoritative) pass, which cannot tell `a[i]` from `a[j]` in `selected`:
// a possible finding there, and nothing else.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=QUIET %s
// QUIET-NOT: {{warning|error}}:
// QUIET: rfc0016-clean.c:[[@LINE+23]]:61: warning: use of 'a[j]' after it may have been freed [weavec::use-after-free]
// QUIET-NOT: {{warning|error}}:
#include "../Inputs/prelude.h"

static void before(char *a, char *b) { *b = 1; free(a); }
static void once(char *a, char *b) {
  char *saved = a;
  if (saved == b) free(saved);
  else { free(a); free(b); }
}
static void clear(char **a, char **b) {
  free(*a); *a = 0;
  if (*b) **b = 1;
}
static void replace(char **a, char **b) {
  free(*a); *a = malloc(4);
  if (*b) **b = 1;
}
struct box { char *data; int release; };
static void guarded(struct box *a, char *b) {
  if (a->release) free(a->data);
  else *b = 1;
}
static void selected(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
void good(void) {
  char *p = malloc(4); if (!p) return;
  before(p, p + 1);
  p = malloc(4); if (!p) return;
  once(p, p);
  p = malloc(4); if (!p) return;
  clear(&p, &p);
  p = malloc(4); if (!p) return;
  replace(&p, &p); free(p);
  p = malloc(4); if (!p) return;
  struct box b = {p, 0}; guarded(&b, p); free(p);
  p = malloc(4); char *q = malloc(4);
  if (!p || !q) { free(p); free(q); return; }
  char *items[2] = {p, q}; selected(items, 0, 1); free(q);
}
