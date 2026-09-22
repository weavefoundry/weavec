// RFC 0030, section 10.6, gate G8: malloc, calloc and realloc become wrappers with their own signatures (§11).
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-allocators.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-allocators.expected.c %t

void *malloc(unsigned long);
void *calloc(unsigned long, unsigned long);
void *realloc(void *, unsigned long);
void free(void *);
void *make(unsigned long n) {
  void *p = malloc(n);
  void *q = calloc(n, 4);
  p = realloc(p, 2 * n);
  free(q);
  return p;
}
