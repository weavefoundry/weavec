// RFC 0034 detection set, case 03 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: < stdin.txt
#include <stdio.h>
#include <stdlib.h>

#define MAX_ITEMS 8

struct msg {
  unsigned declared; /* count from the header */
  size_t got;        /* items actually present */
  unsigned short *items;
};

static struct msg *msg_read(FILE *f) {
  unsigned declared;
  if (fscanf(f, "%u", &declared) != 1)
    return NULL;
  struct msg *m = calloc(1, sizeof *m);
  if (!m)
    return NULL;
  m->items = malloc(MAX_ITEMS * sizeof *m->items);
  if (!m->items) {
    free(m);
    return NULL;
  }
  unsigned v;
  while (m->got < MAX_ITEMS && fscanf(f, "%u", &v) == 1)
    m->items[m->got++] = (unsigned short)v;
  m->declared = declared;
  return m;
}

static unsigned msg_checksum(const struct msg *m) {
  unsigned sum = 0;
#ifdef FIX
  size_t n = m->declared < m->got ? m->declared : m->got;
#else
  size_t n = m->declared; /* trusts the header */
#endif
  for (size_t i = 0; i < n; i++)
    sum = sum * 31 + m->items[i]; // STOP
  return sum;
}

int main(void) {
  struct msg *m = msg_read(stdin);
  if (!m)
    return 1;
  printf("items=%zu checksum=%u\n", m->got, msg_checksum(m));
  free(m->items);
  free(m);
  return 0;
}
