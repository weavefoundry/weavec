// RFC 0041 amendment 17, RFC 0042 §6: a safe function returns what its
// result declares, so one that returns fewer elements than it declares
// stops at its return (`bounds-conversion`).
// FLAGS: -O1
#include <weavec.h>
#include <stddef.h>

static char small[4];

__attribute__((noinline)) WEAVEC_SAFE char *WEAVEC_COUNTED_BY(n) get(size_t n) {
  (void)n;
  return small; // TRAP: bounds-conversion
}

WEAVEC_SAFE void fill(size_t n) {
  char *p = get(n);
  for (size_t i = 0; i < n; i++)
    p[i] = 'x';
}

int main(int argc, char **argv) {
  (void)argv;
  fill((size_t)argc + 15);
  return 0;
}
