// ASAN
#include <stdlib.h>
struct msg { int len; char data[]; };
int main(int argc, char **argv) {
  (void)argv;
  size_t n = (size_t)argc + 3;
  struct msg *m = malloc(sizeof *m + n);
  if (!m) return 1;
  m->data[n] = 0; // BUG: out-of-bounds
  free(m);
  return 0;
}
