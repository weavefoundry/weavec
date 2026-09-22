// A library-style exported API with no callers in the program: are bugs inside it found?
#include <stdlib.h>
#include <string.h>
struct buf { char *data; size_t len, cap; };
int buf_push(struct buf *b, char c) {
  if (b->len > b->cap) return -1;     /* should be >= */
  // RFC 0030 §7.3: 'b' is Single by A1, so the loads of b->data and b->len on the
  // next line are proven; the subscript, the bug site, has no extent.
  b->data[b->len++] = c; // BUG: out-of-bounds // UNRESOLVED: spatial:unknown-extent
  return 0;
}
void buf_reset(struct buf *b) { free(b->data); b->len = 0; }   /* data dangles */
int buf_first(struct buf *b) { return b->data[0]; }
