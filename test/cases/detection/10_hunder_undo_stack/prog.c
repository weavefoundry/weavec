// RFC 0034 detection set, case 10 (heap underflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 5 6 u u u
#include <stdio.h>
#include <stdlib.h>

struct history {
  int *slots;
  int cap;
  int top;
};

static int hist_init(struct history *h, int cap) {
  h->slots = calloc((size_t)cap, sizeof *h->slots);
  h->cap = cap;
  h->top = 0;
  return h->slots ? 0 : -1;
}

static void hist_push(struct history *h, int v) {
  if (h->top < h->cap)
    h->slots[h->top++] = v;
}

static void hist_undo(struct history *h) {
#ifdef FIX
  if (h->top == 0)
    return;
#endif
  h->top--;
  h->slots[h->top] = 0; // STOP
}

int main(int argc, char **argv) {
  struct history h;
  if (hist_init(&h, 4) != 0)
    return 1;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == 'u')
      hist_undo(&h);
    else
      hist_push(&h, atoi(argv[i]));
  }
  printf("depth=%d\n", h.top);
  free(h.slots);
  return 0;
}
