// RFC 0030 §8.3: longjmp does not return.
// STAGE: S4
// The row is 'longjmp (other, int) -> noreturn', so the dereference after
// 'if (p == NULL) longjmp(...)' is reached only with a non-null 'p' and its null facet is
// proven: the unit has no checked null facet. 'get' does not call setjmp, so the §5.4
// downgrade does not apply.
// EXPECT-LEDGER: /summary/facets/null/checked == 0
#include <setjmp.h>
#include <stddef.h>

static jmp_buf env;

int get(const int *p) {
  if (p == NULL) longjmp(env, 1);
  return *p;
}
