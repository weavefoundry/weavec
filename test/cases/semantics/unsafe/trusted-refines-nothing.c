// RFC 0030 §6.1 and §3.2: inside a region spatial and null facets are trusted(unsafe) and refine nothing.
// STAGE: S3
// The dereference inside the region has spatial and null trusted(unsafe) and no check. A
// trusted dereference does not make 'p' non-null downstream, so the dereference after the
// region keeps a checked null facet.
#include <weavec.h>

int twice(int *p, int i) {
  int a;
  WEAVEC_UNSAFE { a = p[i]; } // TRUSTED: spatial:unsafe // TRUSTED: null:unsafe
  return a + p[0]; // TRAP: nonnull
}
