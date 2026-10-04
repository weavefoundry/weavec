// RFC 0033 §4: a guard asks about the bytes the access touches. A pointer formed outside the
// buffer (zstd's `match = base + matchIndex` with a dictionary) and subscripted back into it
// is no trap, wherever the out-of-buffer pointer happens to land.
// STAGE: S3
// CLEAN
// RUN-INPUT: 17 40
// RUN-INPUT: 300 40
// RUN-INPUT: 1000 40
// RUN-INPUT: 4096 40
// RUN-INPUT: 70000 9
#include <stdlib.h>
#include <string.h>
struct window { const unsigned char *base; unsigned start, limit; };
/* Compares from `skip` bytes into the match: the bytes before `skip` are known equal. */
static unsigned match_length(const struct window *w, unsigned index, unsigned skip,
                             unsigned at, unsigned max) {
  const unsigned char *match = w->base + index;
  const unsigned char *ip = w->base + at;
  unsigned n = skip;
  while (n < max && index + n < w->limit && match[n] == ip[n]) n++;
  return n;
}
int main(int argc, char **argv) {
  if (argc < 3) return 2;
  unsigned start = (unsigned)atoi(argv[1]);
  unsigned len = (unsigned)atoi(argv[2]);
  unsigned char *before = malloc(256);
  unsigned char *buf = malloc(256);
  if (buf == NULL || before == NULL) return 1;
  free(before);
  memset(buf, 'a', 256);
  struct window w = {buf - start, start, start + 256};
  /* The match starts 8 bytes before the buffer; the first 8 are skipped. */
  unsigned n = match_length(&w, start - 8, 8, start + 100, len);
  free(buf);
  return n == len ? 0 : 1;
}
