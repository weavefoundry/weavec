// RFC 0030 §11: a pointer read from lowered heap storage is null, so its use traps.
// STAGE: S5
// The call to malloc is lowered to the zeroing wrapper, so the never-written field 'next'
// reads as null and the dereference's nonnull check traps instead of following garbage
// (probe c12's mechanism).
// RUN-INPUT:
#include <stdlib.h>

struct node { struct node *next; int v; };

int main(int argc, char **argv) {
  struct node *n = malloc(sizeof *n);
  (void)argv;
  if (!n) return 1;
  n->v = argc;
  int r = n->next->v; // TRAP: nonnull
  free(n);
  return r;
}
