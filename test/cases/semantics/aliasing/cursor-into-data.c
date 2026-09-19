// RFC 0030 §3.1: a pointer loaded from a parameter-rooted place may alias a released object.
// STAGE: S3
// 'b->cur' points into 'b->data'. After free(b->data), 'b->cur' is loaded from a place the
// analysis cannot prove distinct from the released object: 'cur' is not an owning place,
// the types are compatible and no place identity separates them. So b->cur[0] has temporal
// unresolved(may-alias-released) instead of proven. ASan reports the write.
// ASAN
#include <stdlib.h>

struct buf { char *data; char *cur; };

void reset(struct buf *b) {
  free(b->data);
  b->cur[0] = 0; // BUG: use-after-free // UNRESOLVED: temporal:may-alias-released
  b->data = NULL;
  b->cur = NULL;
}

int main(void) {
  struct buf b;
  b.data = malloc(8);
  if (!b.data) return 1;
  b.cur = b.data + 2;
  reset(&b);
  return 0;
}
