// Unknown external function (no body, no annotation) actually frees its argument.
// Its definition lives in 14_extern_impl.c which is NOT given to the analysis.
// UNITS: 14_extern_impl.c
// ASAN
#include <stdlib.h>
void consume_buffer(char *p);
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  consume_buffer(p);
  return p[0]; // BUG: use-after-free // NOT-PROVEN: temporal
}
