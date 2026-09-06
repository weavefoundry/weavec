// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(size_t n) {
  if (!n) return;
  char local[n];
  local[n] = 0; // BUG: vla-known-miss
}
