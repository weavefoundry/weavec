// Flexible array member overflow.
// ASAN
#include <stdlib.h>
#include <string.h>
struct msg { int len; char data[]; };
int main(int argc, char **argv) {
  (void)argv;
  struct msg *m = malloc(sizeof *m + 4);
  if (!m) return 1;
  m->len = 4;
  for (int i = 0; i <= m->len; i++) m->data[i] = (char)argc; // BUG: out-of-bounds // TRAP: index
  int r = m->data[0];
  free(m);
  return r;
}
