// Tagged union: tag says pointer but integer member was written.
// ASAN
#include <stdint.h>
struct val { int tag; union { int *p; intptr_t n; } u; };
static int get(struct val *v) { return v->tag == 1 ? *v->u.p : (int)v->u.n; } // BUG: out-of-bounds // NOT-PROVEN: spatial
int main(int argc, char **argv) {
  (void)argv;
  struct val v;
  v.u.n = argc * 4096;
  v.tag = 1;
  return get(&v);
}
