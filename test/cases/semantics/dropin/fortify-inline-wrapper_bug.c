// RFC 0033 §5: a library function the C library's header defines inline, to
// call its checking builtin (glibc's `_FORTIFY_SOURCE`), is still the library
// function: its row decides the call, and the length past the destination's
// heap block traps.
// STAGE: S8
// FLAGS: -O1
// RUN-INPUT: 64
#include "Inputs/inline-fortify.h"
#include <stdlib.h>
struct packet { size_t len; unsigned char *payload; };
static void fill(struct packet *pk, const unsigned char *src) {
  memcpy(pk->payload, src, pk->len); // BUG: out-of-bounds // TRAP: object
}
int main(int argc, char **argv) {
  static unsigned char src[128];
  struct packet pk;
  if (argc < 2) return 2;
  pk.len = (size_t)atoi(argv[1]);
  pk.payload = malloc(16);
  if (pk.payload == NULL) return 1;
  fill(&pk, src);
  int r = pk.payload[0];
  free(pk.payload);
  return r;
}
