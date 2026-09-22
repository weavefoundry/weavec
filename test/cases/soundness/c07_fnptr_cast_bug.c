// Function pointer cast to an incompatible type and called.
// ASAN
#include <stdlib.h>
static void free_it(void *p) { free(p); }
typedef void (*borrow_fn)(const char *);
int main(void) {
  char *p = calloc(8, 1);
  if (!p) return 1;
  borrow_fn fn = (borrow_fn)free_it;
  fn(p);
  return p[0]; // BUG: use-after-free
}
