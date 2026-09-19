// Engine pin converted from test/Analysis/rfc0014-pointer-identity.c; markers are the v0.10.0 golden diagnostics.
// RFC 0014: actual callbacks, pointer predicates, complete memory copies.
// RFC 0030 (*Diagnostics*, §15 item 3): `analysis-incomplete` is removed; each
// such pin now has the ledger row that replaces it (`UNRESOLVED`), and is
// listed in test/cases/KNOWN-DIFFERENCES.md.
#include <stdlib.h>
#include <string.h>

static void keep(void *p) { (void)p; }
static void drop(void *p) { free(p); }
static void invoke(void (*fn)(void *), void *p) { fn(p); }
void (*unrelated)(void *) = keep;

void callback_bad(int *p) {
  invoke(drop, p);
  *p = 1; // BUG: use-after-free
}

static void release_same(int *p, int *q) {
  if (p == q)
    free(p);
}
void equality_bad(int *p) {
  release_same(p, p);
  free(p); // BUG: double-free
}

void copied_pointer_bad(int *p) {
  int *q;
  memcpy(&q, &p, sizeof p);
  free(p);
  *q = 1; // BUG: use-after-free
}

void partial(int **dest, int **source) {
  memcpy(dest, source, 1); // UNRESOLVED: temporal:raw-cast
}

struct first { int *p; };
struct second { int tag; int *p; };
static void release_field(void *object) {
  struct first *p = object;
  free(p->p);
}
void incompatible(struct second *p) {
  release_field(p); // UNRESOLVED: temporal:raw-cast
}
