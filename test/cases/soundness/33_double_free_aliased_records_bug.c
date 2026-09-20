// Double free: two owners destroyed independently.
// ASAN
#include <stdlib.h>
struct obj { char *data; };
static struct obj *obj_new(char *shared) { struct obj *o = malloc(sizeof *o); if (!o) return NULL; o->data = shared; return o; }
static void obj_free(struct obj *o) { if (!o) return; free(o->data); free(o); }
int main(void) {
  char *d = malloc(16);
  if (!d) return 1;
  struct obj *a = obj_new(d);
  struct obj *b = obj_new(d);
  obj_free(a);
  obj_free(b); // BUG: double-free
  return 0;
}
