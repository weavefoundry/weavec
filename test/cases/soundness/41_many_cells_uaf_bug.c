// Beyond 32 tracked cells: free element 40, then read it.
// `core::MaxArrayCells` is the engine's budget for the cells of one array
// (§7.4), and 64 elements exceed it: the filling loop stops with "array
// element limit reached" and nothing is known of 'items[40]' afterwards, so
// neither the free nor the read is tracked. Raising the budget only moves the
// cliff, and RFC 0030 has no summary of a cell beyond it, so this stays a
// miss. ASan sees the real defect, and the temporal facet of the read is
// unresolved(budget), so nothing is proven of it.
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[64];
  for (int i = 0; i < 64; i++) { items[i] = malloc(4); if (!items[i]) return 1; items[i][0] = 1; }
  free(items[40]);
  int r = items[40][0]; // MISS: 64 cells exceed the per-array cell budget, so nothing is tracked for items[40]
  for (int i = 0; i < 64; i++) if (i != 40) free(items[i]);
  return r;
}
