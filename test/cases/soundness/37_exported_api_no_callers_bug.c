// A library-style exported API with no callers in the program: are bugs inside it found?
#include <stdlib.h>
#include <string.h>
struct buf { char *data; size_t len, cap; };
int buf_push(struct buf *b, char c) {
  if (b->len > b->cap) return -1;     /* should be >= */
  b->data[b->len++] = c; // BUG: out-of-bounds // NOT-PROVEN: spatial
  return 0;
}
void buf_reset(struct buf *b) { free(b->data); b->len = 0; }   /* data dangles */
int buf_first(struct buf *b) { return b->data[0]; }
