// RFC 0030 §4 "Checked, index": a declared counted pointer gets an index and a nonnull check.
// STAGE: S6
// p[i] has spatial checked with the index template against the declared count 'n', null
// checked and temporal proven. Emitted:
// ((const int *)__weavec_chk_nonnull(p))[__weavec_chk_index(i, n)].
// The first run passes index 4 of 4 elements, the second a null pointer with index 0.
// RUN-INPUT: 4
// RUN-INPUT: 0 null
#include <stdlib.h>
#include "../Inputs/rfc0030.h"

int at(const int *WEAVEC_COUNTED_BY(n) p, size_t n, size_t i) { return p[i]; } // TRAP: index // TRAP: nonnull

int main(int argc, char **argv) {
  int xs[4] = {1, 2, 3, 4};
  const int *p = argc > 2 ? NULL : xs;
  size_t i = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 0;
  return at(p, 4, i);
}
