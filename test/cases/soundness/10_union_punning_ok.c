// CLEAN
// ASAN
#include <stdint.h>
union pun { int *p; uintptr_t bits; };
int main(int argc, char **argv) {
  (void)argv;
  int x = argc;
  union pun u;
  u.p = &x;
  return *u.p;
}
