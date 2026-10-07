// RFC 0031 §4.2: a variable index uses the array's summary cell (weak); a release through it may reach every element, so a later use of any element is not proven.
// STAGE: S2
// Every element of 'a' is stored and released through 'a[i]'; 'a[0]' is read afterwards
// (ASan: heap-use-after-free). The finding may be possible rather than definite.
// ASAN
#include <stdlib.h>
struct n { int v; };
int main(void) {
  struct n *a[4];
  for (int i = 0; i < 4; i++) {
    a[i] = malloc(sizeof *a[i]);
    if (!a[i]) abort();
    a[i]->v = i;
  }
  for (int i = 0; i < 4; i++)
    free(a[i]);
  struct n *first = a[0];
  return first->v; // BUG: use-after-free
}
