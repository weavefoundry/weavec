// RFC 0033 §5: a fortified memcpy (`__builtin___memcpy_chk` with an unknown object size) is
// guarded like memcpy: a length read from a field, past the destination's heap block, traps.
// STAGE: S3
// FLAGS: -O1 -D_FORTIFY_SOURCE=2
// RUN-INPUT: 64
#include <stdlib.h>
#include <string.h>
struct packet { size_t len; unsigned char *payload; };
static void fill(struct packet *pk, const unsigned char *src) {
  memcpy(pk->payload, src, pk->len); // BUG: out-of-bounds // TRAP
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
