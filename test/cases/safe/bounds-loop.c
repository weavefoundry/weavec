// RFC 0041 §4: a loop over a counted parameter one element too far stops at
// the access with pointer-out-of-bounds; the guard it replaced is in the
// ledger as proven `safe`.
// FLAGS: -O2
#include <weavec.h>
#include <stddef.h>

WEAVEC_SAFE long sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  long s = 0;
  for (size_t i = 0; i <= n; i++)
    s += v[i]; // TRAP: pointer-out-of-bounds
  return s;
}

int main(void) {
  int a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  return (int)sum(a, 8);
}
