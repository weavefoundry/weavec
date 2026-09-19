// RFC 0030 §7.2 (counted_by, counted_by_or_null, sized_by, sized_by_or_null): Clang's bounds attributes declare kinds.
// STAGE: S6
// 'buf' is Counted(len) and non-null (counted_by, not the _or_null form). p->buf[i] with an
// unknown 'i' is checked with the index template against 'p->len'. The run asks for index
// 4 of 4.
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>

struct pkt { int len; char *buf __attribute__((counted_by(len))); };

char at(const struct pkt *p, int i) { return p->buf[i]; } // BUG: out-of-bounds // TRAP: index

int main(int argc, char **argv) {
  char storage[4] = "abc";
  struct pkt p = {4, storage};
  return at(&p, argc > 1 ? atoi(argv[1]) : 0);
}
