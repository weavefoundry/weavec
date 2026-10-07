// RFC 0032 §2.3: a reallocation that moves a block releases the old one, so a pointer kept to it is caught.
// STAGE: S3
// 'first' is taken before 'append' grows the buffer into another size class.
// RUN-INPUT: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
// ASAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct buf { char *p; size_t len, cap; };
static void append(struct buf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
    if (!b->p) abort();
  }
  memcpy(b->p + b->len, s, n + 1);
  b->len += n;
}
int main(int argc, char **argv) {
  struct buf b = {0};
  append(&b, "hello");
  char *first = b.p;
  for (int i = 1; i < argc; i++) append(&b, argv[i]);
  int r = first[0] != 'h'; // BUG: use-after-move possible // TRAP
  free(b.p);
  return r;
}
