// RFC 0017: shared numeric interfaces, analyzed across translation units.
#include "numeric-helpers.h"
unsigned char narrow_count(unsigned n) { return n; }
void narrow_count_out(unsigned n, size_t *out) { *out = (unsigned char)n; }
void fill_min(char *p, size_t n, size_t cap) {
  for (size_t i = 0; i < n && i < cap; ++i) p[i] = 0;
}
int checked_size(size_t n, size_t m, size_t *out) {
  return __builtin_mul_overflow(n, m, out);
}
