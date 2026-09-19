// Engine pin converted from test/Analysis/rfc0007-leaks.c; markers are the v0.10.0 golden diagnostics.
// RFC 0007, *Leaks*: an owned resource whose every holder goes out of reach
// without being released, moved or escaped is reported once, at the point
// it is lost (a return, a scope end, an overwrite, a discarded call).
#include "Inputs/prelude.h"
#include <weavec.h>

char *strdup(const char *s);

struct buf {
  char *data;
  size_t n;
};

// The RFC's snippets that must be reported.

int leak_path(int c) {
  char *p = malloc(8);
  if (c)
    return -1; // BUG: leak possible
  free(p);
  return 0;
}

void overwrite(void) {
  char *p = malloc(8);
  p = malloc(16); // BUG: leak possible
  free(p);
}

void owned_param(char *WEAVEC_OWNED p) {
  use(p); // BUG: leak possible
}

void discarded(const char *s) {
  strdup(s); // BUG: leak possible
}

// A value that is never read is lost right after it is stored.
int never_used(int c) {
  char *p = malloc(8);
  if (c) // BUG: leak possible
    return 1;
  return 2;
}

// The record travels with copies: the leak is reported for the holder that
// dies last, and there is no second report for `p`.
void copies(void) {
  char *p = malloc(8);
  char *q = p;
  use(q); // BUG: leak possible
}

// Overwriting an owned global loses the old value for this function.
static char *global;
void global_overwrite(void) {
  global = malloc(8);
  global = malloc(16); // BUG: leak possible
}

// A field the function itself made owned is checked on overwrite.
void field_overwrite(struct buf *b) {
  b->data = malloc(8);
  b->data = malloc(16); // BUG: leak possible
}

// Once a merge-point false positive: `p` may own at the second `if`, but the
// resource is held under the guard `c != 0`, which the early return's edge
// refutes (RFC 0009, *Refuting guards in the state*).
int merged(int c) {
  char *p = NULL;
  if (c)
    p = malloc(8);
  if (!c)
    return -1;
  free(p);
  return 0;
}
