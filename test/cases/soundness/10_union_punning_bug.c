// Union punning: integer written, pointer member dereferenced.
// ASAN
#include <stdint.h>
union pun { int *p; uintptr_t bits; };
int main(int argc, char **argv) {
  (void)argv;
  union pun u;
  u.bits = (uintptr_t)argc * 0x1000u;
  return *u.p; // BUG: out-of-bounds // NOT-PROVEN: spatial
}
