// ASAN
#include <stdlib.h>
struct t { struct t *l, *r; int v; };
static void t_free(struct t *n) { if (!n) return; t_free(n->l); t_free(n->r); free(n); }
int main(void) {
  struct t *a = calloc(1, sizeof *a);
  if (!a) return 1;
  a->l = calloc(1, sizeof *a);
  struct t *keep = a->l;
  t_free(a);
  return keep ? keep->v : 0; // BUG: use-after-free
}
