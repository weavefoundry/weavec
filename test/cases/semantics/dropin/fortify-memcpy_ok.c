// RFC 0033 §5: the fortified memcpy within its block runs.
// STAGE: S3
// FLAGS: -O1 -D_FORTIFY_SOURCE=2
// CLEAN
// RUN-INPUT: 16
#include <stdlib.h>
#include <string.h>
struct packet { size_t len; unsigned char *payload; };
static void fill(struct packet *pk, const unsigned char *src) {
  memcpy(pk->payload, src, pk->len);
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
