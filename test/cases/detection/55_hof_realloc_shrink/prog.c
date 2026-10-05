// RFC 0034 detection set, case 55 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: secret token value
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sbuf {
  char *data;
  size_t len, cap;
};

static int sb_init(struct sbuf *b, size_t cap) {
  b->data = calloc(1, cap);
  b->len = 0;
  b->cap = cap;
  return b->data ? 0 : -1;
}

static void sb_append(struct sbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap)
    n = b->cap - b->len - 1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void sb_compact(struct sbuf *b) {
  char *nd = realloc(b->data, b->len + 1);
  if (!nd)
    return;
  b->data = nd;
#ifdef FIX
  b->cap = b->len + 1;
#endif
}

static void sb_wipe(struct sbuf *b) {
  memset(b->data, 0, b->cap); // STOP
  b->len = 0;
}

int main(int argc, char **argv) {
  struct sbuf b;
  if (sb_init(&b, 256))
    return 1;
  for (int i = 1; i < argc; i++)
    sb_append(&b, argv[i]);
  sb_compact(&b);
  printf("%s (%zu)\n", b.data, b.len);
  sb_wipe(&b);
  free(b.data);
  return 0;
}
