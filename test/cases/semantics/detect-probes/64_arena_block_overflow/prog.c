// RFC 0034 detection probe, case 64 (arena sub-allocation): a probe written after the 61 blind programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: the quick brown fox jumps over the lazy dog
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* follow-up probe written after case 57: the overflow leaves the arena's block */
struct arena {
  char *base;
  size_t used, cap;
};

static void *arena_alloc(struct arena *a, size_t n) {
#ifdef FIX
  if (a->used + n > a->cap)
#else
  if (a->used > a->cap) /* checks the start, not the end */
#endif
    return NULL;
  void *p = a->base + a->used;
  a->used += n;
  return p;
}

int main(int argc, char **argv) {
  struct arena a = {malloc(32), 0, 32};
  if (!a.base)
    return 1;
  char *words[16];
  int nw = 0;
  for (int i = 1; i < argc && nw < 16; i++) {
    char *t = arena_alloc(&a, strlen(argv[i]) + 1);
    if (!t)
      break;
    strcpy(t, argv[i]); // STOP
    words[nw++] = t;
  }
  for (int i = 0; i < nw; i++)
    printf("%s ", words[i]);
  printf("(%zu bytes)\n", a.used);
  free(a.base);
  return 0;
}
