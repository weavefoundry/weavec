// RFC 0033 §5: the fortified strcat within its block runs.
// STAGE: S3
// FLAGS: -O1 -D_FORTIFY_SOURCE=2
// CLEAN
// RUN-INPUT: 0123
#include <stdlib.h>
#include <string.h>
struct builder { char *data; size_t cap; };
static void append(struct builder *b, const char *s) { strcat(b->data, s); }
int main(int argc, char **argv) {
  struct builder b;
  if (argc < 2) return 2;
  b.cap = 16;
  b.data = calloc(1, b.cap);
  if (b.data == NULL) return 1;
  append(&b, "id:");
  append(&b, argv[1]);
  int r = b.data[0];
  free(b.data);
  return r == 'i' ? 0 : 1;
}
