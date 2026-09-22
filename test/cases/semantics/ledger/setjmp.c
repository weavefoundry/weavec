// RFC 0030 §5.4: in a function that calls setjmp, every temporal facet is unresolved(setjmp).
// STAGE: S3
// Values may be stale after a longjmp, so every temporal facet of 'run' is
// unresolved(setjmp) and the ledger marks the function. A null facet that a flow fact about
// a local would prove (the test of 'p') is checked instead. Facets proven by type alone,
// such as a constant index into the local array, stay proven.
// EXPECT-LEDGER: /units/0/functions/0/setjmp == true
#include <setjmp.h>
#include <stddef.h>

static jmp_buf env;

int run(char *p) {
  char local[4] = {0};
  if (p == NULL) return -1;
  if (setjmp(env) != 0) return p[0] + local[1]; // UNRESOLVED: temporal:setjmp // TRAP: nonnull
  p[0] = 1; // UNRESOLVED: temporal:setjmp
  local[1] = 2;
  longjmp(env, 1);
}
