// RFC 0030 §4 "Violation, null": a dereference on the null edge of a test is an error.
// STAGE: S3
// On the null edge 'n' is Null with reason Tested, not from an allocator, so the result is
// "dereference of 'n', which is null [weavec::null-dereference]".
// ASAN
#include <stddef.h>

struct node { struct node *next; int v; };

int k(struct node *n) { if (n == NULL) return n->v; return 0; } // BUG: null-dereference definite

int main(int argc, char **argv) {
  struct node x = {NULL, 1};
  (void)argv;
  return k(argc > 5 ? &x : NULL);
}
