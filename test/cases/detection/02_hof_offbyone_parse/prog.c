// RFC 0034 detection set, case 02 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 3,1,4,1,5
#include <stdio.h>
#include <stdlib.h>

struct series {
  int *vals;
  size_t n;
};

static int series_parse(struct series *s, const char *csv) {
  size_t n = 1;
  for (const char *p = csv; *p; p++)
    if (*p == ',')
      n++;
  s->vals = malloc(n * sizeof *s->vals);
  if (!s->vals)
    return -1;
  s->n = n;
  const char *p = csv;
#ifdef FIX
  for (size_t i = 0; i < n; i++) {
#else
  for (size_t i = 0; i <= n; i++) {
#endif
    char *end;
    s->vals[i] = (int)strtol(p, &end, 10); // STOP
    p = end;
    if (*p == ',')
      p++;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct series s;
  if (series_parse(&s, argv[1]) != 0)
    return 1;
  long sum = 0;
  for (size_t i = 0; i < s.n; i++)
    sum += s.vals[i];
  printf("n=%zu mean=%.2f\n", s.n, (double)sum / (double)s.n);
  free(s.vals);
  return 0;
}
