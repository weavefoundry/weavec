// RFC 0030 §7.2 (WEAVEC_COUNTED_BY on a field): accesses through the field are checked against it.
// STAGE: S6
// 'data' is Counted(cap). b->data[i] with an unknown 'i' is checked with the index template
// against 'b->cap' (§7.4 rule 2). The run asks for index 8 of 8.
// RUN-INPUT: 8
// ASAN
#include <stdlib.h>
#include "../Inputs/rfc0030.h"

struct buf { char *WEAVEC_COUNTED_BY(cap) data; size_t cap; };

char at(const struct buf *b, size_t i) { return b->data[i]; } // BUG: out-of-bounds // TRAP: index

int main(int argc, char **argv) {
  char storage[8] = "abcdefg";
  struct buf b = {storage, sizeof storage};
  size_t i = argc > 1 ? (size_t)atoi(argv[1]) : 0;
  return at(&b, i);
}
