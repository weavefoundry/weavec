// A program with its own `reallocarray` over `realloc`, as portability
// layers have (runtime-link.c): it replaces the runtime's weak definition
// and the program keeps the runtime's allocator.
#include <stdio.h>
#include <stdlib.h>

void *reallocarray(void *p, size_t count, size_t size) {
  fputs("shim\n", stderr);
  return realloc(p, count * size);
}

int main(void) {
  char *p = reallocarray(0, 2, 8);
  if (!p)
    return 1;
  p[0] = 1;
  free(p);
  return 0;
}
