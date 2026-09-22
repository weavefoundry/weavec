// RFC 0030, section 10.6, gate G8: taking an allocator's address yields a static non-inline wrapper.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-address.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-address.expected.c %t

void *malloc(unsigned long);
void *(*allocate)(unsigned long) = malloc;
void *make(unsigned long n) {
  void *(*f)(unsigned long) = malloc;
  return f(n);
}
