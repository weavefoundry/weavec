// RFC 0033 §5: a copy into a flexible array member whose length is a string's is guarded.
// STAGE: S3
// RUN-INPUT: a-much-longer-message-than-was-allocated
// ASAN
#include <stdlib.h>
#include <string.h>
struct message { size_t len; char data[]; };
static void put(struct message *m, const char *p) {
  memcpy(m->data, p, strlen(p)); // BUG: out-of-bounds // TRAP
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  struct message *m = malloc(sizeof *m + 8);
  if (m == NULL) return 1;
  m->len = 8;
  put(m, argv[1]);
  int r = m->data[0];
  free(m);
  return r;
}
