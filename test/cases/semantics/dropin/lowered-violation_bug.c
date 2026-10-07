// RFC 0033 (V): a lowered definite violation is guarded, so it traps when the bug happens and
// not before; the guard's template is the runtime's, not the unconditional `violation`.
// STAGE: S1
// FLAGS: -Wno-error=weavec-use-after-free
// RUN-INPUT: 1
#include <stdlib.h>
static int run(int n) {
  int *p = malloc(sizeof *p);
  if (p == NULL) return 1;
  *p = n;
  free(p);
  if (n > 0) return *p; // BUG: use-after-free // TRAP
  return 0;
}
int main(int argc, char **argv) { return run(argc > 1 ? atoi(argv[1]) : 0); }
