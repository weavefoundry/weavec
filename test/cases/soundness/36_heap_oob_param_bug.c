// Helper writes n bytes; caller passes a smaller heap buffer (count from input).
// ASAN
#include <stdlib.h>
static void fill(char *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = 0; } // TRAP
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(4);
  if (!p) return 1;
  fill(p, (size_t)argc + 4); // BUG: out-of-bounds // MISS: the analysis does not report a call's argument against the callee's requirement (RFC 0035 §8); the guard stops in the callee
  free(p);
  return 0;
}
