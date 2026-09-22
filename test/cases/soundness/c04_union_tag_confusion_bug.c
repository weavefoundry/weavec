// Tagged union: tag says pointer but integer member was written.
// ASAN
#include <stdint.h>
struct val { int tag; union { int *p; intptr_t n; } u; };
// RFC 0030 §7.3: 'v' is Single, so the loads through it on the next line are proven;
// the slot 'val.u.p' shares its union with an integer, so *v->u.p has no extent.
static int get(struct val *v) { return v->tag == 1 ? *v->u.p : (int)v->u.n; } // BUG: out-of-bounds // UNRESOLVED: spatial:unknown-extent
int main(int argc, char **argv) {
  (void)argv;
  struct val v;
  v.u.n = argc * 4096;
  v.tag = 1;
  return get(&v);
}
