// RFC 0034 detection probe, case 62 (use after free): a probe written after the 61 blind programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: ok ok cancel help
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* follow-up probe written after case 24 missed: alias kept in a local */
struct widget {
  int x, y;
  char label[16];
};

struct panel {
  struct widget *kids[8];
  int n;
};

static void panel_add(struct panel *p, const char *label, int x) {
  if (p->n == 8)
    return;
  struct widget *w = calloc(1, sizeof *w);
  if (!w)
    exit(1);
  w->x = x;
  snprintf(w->label, sizeof w->label, "%s", label);
  p->kids[p->n++] = w;
}

static void panel_close(struct panel *p, const char *label) {
  for (int i = 0; i < p->n; i++) {
    if (strcmp(p->kids[i]->label, label) == 0) {
      free(p->kids[i]);
      p->kids[i] = p->kids[--p->n];
      return;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct panel p = {{0}, 0};
  for (int i = 2; i < argc; i++)
    panel_add(&p, argv[i], i * 10);
  struct widget *focused = p.kids[0];
#ifdef FIX
  int focused_closed = strcmp(focused->label, argv[1]) == 0;
#endif
  panel_close(&p, argv[1]);
#ifdef FIX
  if (focused_closed)
    focused = p.n ? p.kids[0] : NULL;
  if (focused)
#endif
    printf("focus at x=%d\n", focused->x); // STOP
  for (int i = 0; i < p.n; i++)
    free(p.kids[i]);
  return 0;
}
