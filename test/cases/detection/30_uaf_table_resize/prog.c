// RFC 0034 detection set, case 30 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: the cat sat on the mat
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
  char word[16];
  int count;
  int followers;
};

struct table {
  struct entry *e;
  size_t n, cap;
};

static struct entry *table_get(struct table *t, const char *w) {
  for (size_t i = 0; i < t->n; i++)
    if (strcmp(t->e[i].word, w) == 0)
      return &t->e[i];
  if (t->n == t->cap) {
    size_t ncap = t->cap ? t->cap * 2 : 2;
    struct entry *ne = realloc(t->e, ncap * sizeof *ne);
    if (!ne)
      return NULL;
    t->e = ne;
    t->cap = ncap;
  }
  struct entry *e = &t->e[t->n++];
  memset(e, 0, sizeof *e);
  snprintf(e->word, sizeof e->word, "%s", w);
  return e;
}

int main(int argc, char **argv) {
  struct table t = {0};
#ifdef FIX
  long last = -1;
#else
  struct entry *last = NULL;
#endif
  for (int i = 1; i < argc; i++) {
    struct entry *e = table_get(&t, argv[i]);
    if (!e)
      return 1;
    e->count++;
#ifdef FIX
    if (last >= 0)
      t.e[last].followers++;
    last = e - t.e;
#else
    if (last)
      last->followers++; // STOP
    last = e;
#endif
  }
  for (size_t i = 0; i < t.n; i++)
    printf("%s %d %d\n", t.e[i].word, t.e[i].count, t.e[i].followers);
  free(t.e);
  return 0;
}
