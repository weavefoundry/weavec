// RFC 0017: checked multiplication returns overflow and stores SIZE_MAX - 1.
#include "numeric-helpers.h"
void run(size_t rows) {
  if (rows != (size_t)-1) return;
  size_t bytes;
  int overflow = checked_size(rows, 2, &bytes);
  char *p = malloc(1);
  if (!p) return;
  free(p);
  if (!overflow || bytes != (size_t)-2) *p = 1;
}
