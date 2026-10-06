// RFC 0034 detection set, case 39 (uninitialised pointer): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 50
#include <stdio.h>
#include <stdlib.h>

struct cand {
  const char *name;
  int score;
};

static const struct cand *pick(const struct cand *c, int n, int threshold) {
#ifdef FIX
  const struct cand *best = NULL;
#else
  const struct cand *best;
#endif
  int best_score = threshold;
  for (int i = 0; i < n; i++) {
    if (c[i].score > best_score) {
      best = &c[i];
      best_score = c[i].score;
    }
  }
  return best;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct cand cands[3] = {{"ann", 10}, {"ben", 25}, {"cal", 17}};
  const struct cand *b = pick(cands, 3, atoi(argv[1]));
#ifdef FIX
  if (!b) {
    printf("nobody above threshold\n");
    return 0;
  }
#endif
  printf("winner %s with %d\n", b->name, b->score); // STOP
  return 0;
}
