// RFC 0030, section 10.6, gate G8: posix_memalign is called through its helper, which zeroes the block.
// `&p` is non-null and points to one whole pointer, so the call has no check
// of its own.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-posix-memalign.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-posix-memalign.expected.c %t

int posix_memalign(void **, unsigned long, unsigned long);
void *get(unsigned long n) {
  void *p = 0;
  if (posix_memalign(&p, 64, n) != 0)
    return 0;
  return p;
}
