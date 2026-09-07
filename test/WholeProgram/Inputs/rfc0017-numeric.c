// RFC 0017: typed numeric and spatial interfaces across translation units.
#include "../../Inputs/prelude.h"
unsigned char narrow(unsigned n) { return n; }
void narrow_out(unsigned n, unsigned char *out) { *out = n; }
void put_at(char *p, int i) { p[i] = 1; }
void fill_min(char *p, size_t n, size_t cap) {
  for (size_t i = 0; i < n && i < cap; ++i) p[i] = 1;
}
char *make_product(size_t rows, size_t cols) { return malloc(rows * cols); }
int checked_size(size_t n, size_t m, size_t *out) {
  return __builtin_mul_overflow(n, m, out);
}
void reverse_outputs(unsigned *a, unsigned *b) { *b = 1; *a = 2; }
void release_numeric_pointer(void *p) { free(p); }
void *checked_allocation(size_t n, size_t m) {
  void *calloc(size_t, size_t);
  return calloc(n, m);
}
