// RFC 0030 §5.1: a callee whose pointer parameters all carry ownership contracts applies them.
// STAGE: S3
// 'inspect' is defined elsewhere, but its only pointer parameter is WEAVEC_BORROWED, an
// ownership contract: the call borrows 'p' and nothing else happens to it. The Call site's
// temporal facet is trusted(extern-contract), and the later use and release are not
// unresolved.
// CLEAN
#include <stdlib.h>
#include <weavec.h>

void inspect(const char *WEAVEC_BORROWED p);

int f(void) {
  char *p = malloc(8);
  if (!p) return 0;
  p[0] = 1;
  inspect(p);
  int v = p[0];
  free(p);
  return v;
}
