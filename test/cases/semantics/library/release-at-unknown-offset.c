// RFC 0031 *Implementation amendments*, "Releases at an unknown offset".
// STAGE: S7
// `sfree` releases `s - hdr_size(s[-1])` (hiredis's `sdsfree`): an offset
// into the object `s` points into that it does not know. Its summary said
// `release *param0` at offset 0, so `main` released `c`, three bytes into
// its allocation: a definite (and false) `invalid-release`. It says
// `offset=?` now, and the caller cannot tell where the released pointer
// points: a possible finding. The header `snew` writes before its result is
// not described either, so the caller reads it as unknown, not as zeros.
// CLEAN
// ALLOW: invalid-release
// ASAN
// RUN-INPUT:
#include <stdlib.h>

struct __attribute__((packed)) hdr8 {
  unsigned char len, alloc, flags;
  char buf[];
};

static int hdr_size(char type) {
  switch (type & 7) {
  case 0:
    return 1;
  case 1:
    return 3;
  case 2:
    return 5;
  }
  return 0;
}

static char *snew(void) {
  struct hdr8 *sh = malloc(4);
  if (!sh)
    return NULL;
  char *s = (char *)sh + 3;
  unsigned char *fp = ((unsigned char *)s) - 1;
  sh->len = 0;
  sh->alloc = 0;
  *fp = 1;
  s[0] = 0;
  return s;
}

static void sfree(char *s) {
  if (s == NULL)
    return;
  free(s - hdr_size(s[-1]));
}

int main(void) {
  char *c = snew();
  if (!c)
    return 1;
  sfree(c);
  return 0;
}
