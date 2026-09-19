// ASAN
#include <stdlib.h>
struct pair { char *a, *b; };
static void cleanup(struct pair *s) { free(s->a); free(s->b); } // BUG: double-free
int main(void) {
  struct pair s;
  s.a = malloc(8);
  s.b = s.a;
  cleanup(&s);
  return 0;
}
