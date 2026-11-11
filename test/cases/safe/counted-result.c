// RFC 0041 amendment 17: a bounded result is checked against the shadow
// where it is returned to a safe caller, a safe callee's too, so a function
// that returns fewer elements than it declares stops the caller.
// FLAGS: -O1
#include <weavec.h>
#include <stddef.h>

static char small[4];

__attribute__((noinline)) WEAVEC_SAFE char *WEAVEC_COUNTED_BY(n) get(size_t n) {
  (void)n;
  return small;
}

WEAVEC_SAFE void fill(size_t n) {
  char *p = get(n); // TRAP: global-buffer-overflow
  for (size_t i = 0; i < n; i++)
    p[i] = 'x';
}

int main(int argc, char **argv) {
  (void)argv;
  fill((size_t)argc + 15);
  return 0;
}
