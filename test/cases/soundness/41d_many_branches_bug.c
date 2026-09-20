// Many independent branches before a real UAF (path/state explosion).
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 0; p[1] = 0;
  int x = 0;
#define B(k) if (argc & (1 << (k))) x += k; else x -= k;
  B(0) B(1) B(2) B(3) B(4) B(5) B(6) B(7) B(8) B(9) B(10) B(11) B(12) B(13) B(14) B(15)
  B(16) B(17) B(18) B(19) B(20) B(21) B(22) B(23) B(24) B(25) B(26) B(27) B(28) B(29)
  free(p);
  return x + p[argc & 1]; // BUG: use-after-free
}
