// Pointer to a static result buffer invalidated by the next call (not a memory error, logic) +
// getenv result used after setenv may be freed.
// ASAN
#include <stdlib.h>
int main(void) {
  setenv("WEAVEC_PROBE", "first-value", 1);
  const char *v = getenv("WEAVEC_PROBE");
  setenv("WEAVEC_PROBE", "a-much-longer-second-value-to-force-reallocation", 1);
  return v ? v[0] : 0; // BUG: use-after-free
}
