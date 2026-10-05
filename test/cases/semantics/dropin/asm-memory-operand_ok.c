// RFC 0033 amendment 1: an inline-assembly operand taken in memory only
// (`"+m" (*(uint64_t (*)[16])d)`, mbedtls's bn_mul.h) passes an address; it is
// no access of its whole type, so no guard checks 128 bytes behind `d`.
// STAGE: S8
// CLEAN
// RUN-INPUT:
#include <stdint.h>
#include <stdlib.h>
static void touch(uint64_t *d) {
  __asm__ volatile("" : "+m"(*(uint64_t (*)[16])d));
}
int main(void) {
  uint64_t *v = calloc(2, sizeof *v);
  if (v == NULL) return 1;
  touch(v);
  int r = (int)v[1];
  free(v);
  return r;
}
