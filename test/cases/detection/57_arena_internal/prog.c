// RFC 0034 detection set, case 57 (arena sub-allocation): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: let x 42 in x
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct arena {
  char *base;
  size_t used, cap;
};

static void *arena_alloc(struct arena *a, size_t n) {
  if (a->used + n > a->cap)
    return NULL;
  void *p = a->base + a->used;
  a->used += n;
  return p;
}

struct token {
  char *text;
  int kind;
};

int main(int argc, char **argv) {
  struct arena a = {malloc(4096), 0, 4096};
  if (!a.base)
    return 1;
  struct token toks[16];
  int nt = 0;
  for (int i = 1; i < argc && nt < 16; i++) {
#ifdef FIX
    char *t = arena_alloc(&a, strlen(argv[i]) + 1);
#else
    char *t = arena_alloc(&a, strlen(argv[i])); /* no room for the NUL */
#endif
    if (!t)
      break;
    strcpy(t, argv[i]); // STOP // MISS: the strcpy's checked wrapper (RFC 0034 section 5.2) checks the room left in the tracked object, the arena's one heap block; the missing byte is inside it
    toks[nt].text = t;
    toks[nt].kind = argv[i][0] >= '0' && argv[i][0] <= '9';
    nt++;
  }
  for (int i = 0; i < nt; i++)
    printf("%d:%s\n", toks[i].kind, toks[i].text);
  free(a.base);
  return 0;
}
