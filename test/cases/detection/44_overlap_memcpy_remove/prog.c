// RFC 0034 detection set, case 44 (overlapping copy): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 64 3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct queue {
  int *items;
  size_t n;
};

static void queue_remove(struct queue *q, size_t idx) {
  if (idx >= q->n)
    return;
#ifdef FIX
  memmove(&q->items[idx], &q->items[idx + 1], (q->n - idx - 1) * sizeof *q->items);
#else
  memcpy(&q->items[idx], &q->items[idx + 1], (q->n - idx - 1) * sizeof *q->items); // STOP
#endif
  q->n--;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct queue q;
  q.n = (size_t)atoi(argv[1]);
  q.items = malloc(q.n * sizeof *q.items);
  if (!q.items)
    return 1;
  for (size_t i = 0; i < q.n; i++)
    q.items[i] = (int)i * 10;
  queue_remove(&q, (size_t)atoi(argv[2]));
  long s = 0;
  for (size_t i = 0; i < q.n; i++)
    s += q.items[i] * (long)(i + 1);
  printf("n=%zu weighted=%ld\n", q.n, s);
  free(q.items);
  return 0;
}
