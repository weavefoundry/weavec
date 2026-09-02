/* Half of a two-unit cycle with rfc0005-cycle.c: `b_free` frees, and it is
 * reached from the other unit through `a_free`, defined there. */
#include "../../Inputs/prelude.h"

void a_free(void *p);

void b_free(void *p) { free(p); }

void b_release_twice(char *p) {
  a_free(p);
  free(p);
}
