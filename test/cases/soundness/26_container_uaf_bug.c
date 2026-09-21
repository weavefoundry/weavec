// Array of owned pointers: free all in a loop, then read an element.
// The filling loop's body is three statements, and the contiguous-fill
// recogniser of §7.4 takes only a loop whose whole body is the one store, so
// no cell of 'items' ever takes the allocated value: the final read finds
// 'items[2]' as it was declared and is reported as a use of an uninitialized
// pointer, not as the use after free it is. ASan sees the real defect, and
// the temporal facet there is unresolved, so nothing is proven of it.
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[4];
  for (int i = 0; i < 4; i++) { items[i] = malloc(4); if (!items[i]) return 1; items[i][0] = (char)i; }
  for (int i = 0; i < 4; i++) free(items[i]);
  return items[2][0]; // MISS: the filling loop is not recognised, so the cell reads as uninitialized rather than freed
}
