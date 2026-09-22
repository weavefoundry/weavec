// RFC 0030 §7.1: a qsort comparator's 'const struct T *a = pa' is never checked against pa's Single default.
// STAGE: S3
// 'pa' is Single-or-nullable by A1, and Single for 'const void *' guarantees one byte: a
// lower bound. The conversion to 'const struct T *' is not a required position, so 'a' gets
// kind Unknown and a->key is unresolved(unknown-extent). Checking it against the one-byte
// bound would trap every correct comparator. No error, no trap.
// CLEAN
// ASAN
#include <stdlib.h>

struct T { int key; const char *name; };

static int cmp(const void *pa, const void *pb) {
  const struct T *a = pa;
  const struct T *b = pb;
  return (a->key > b->key) - (a->key < b->key); // UNRESOLVED: spatial:unknown-extent
}

int main(void) {
  struct T ts[3] = {{3, "c"}, {1, "a"}, {2, "b"}};
  qsort(ts, 3, sizeof ts[0], cmp);
  return ts[0].key == 1 && ts[2].key == 3 ? 0 : 1;
}
