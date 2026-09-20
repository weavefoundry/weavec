// RFC 0017: checked multiplication returns overflow and stores SIZE_MAX - 1.
// CLEAN
// TOOL
// The helper's overflow result (another unit) makes the use after free
// unreachable; only the whole-program view sees it, so the pair is analysed
// with `weavec --whole-program` (RFC 0030 §Soundness, *Accepted false
// positives*: an infeasible path a per-unit compile cannot refute).
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(size_t rows) {
  if (rows != (size_t)-1) return;
  size_t bytes;
  int overflow = checked_size(rows, 2, &bytes);
  char *p = malloc(1);
  if (!p) return;
  free(p);
  if (!overflow || bytes != (size_t)-2) *p = 1;
}
