// RFC 0034 detection set, case 33 (double free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: Annual_Report 42
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct doc {
  char *title;
  int pages;
};

struct view {
  char *title;
  int width;
};

static struct doc *doc_new(const char *title, int pages) {
  struct doc *d = malloc(sizeof *d);
  if (!d)
    return NULL;
  d->title = strdup(title);
  d->pages = pages;
  return d;
}

static void doc_free(struct doc *d) {
  free(d->title);
  free(d);
}

static struct view *view_for(const struct doc *d, int width) {
  struct view *v = malloc(sizeof *v);
  if (!v)
    return NULL;
#ifdef FIX
  v->title = strdup(d->title);
#else
  v->title = d->title; /* shares the doc's string */
#endif
  v->width = width;
  return v;
}

static void view_free(struct view *v) {
  free(v->title); // STOP
  free(v);
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct doc *d = doc_new(argv[1], atoi(argv[2]));
  if (!d)
    return 1;
  struct view *v = view_for(d, 80);
  if (!v)
    return 1;
  printf("%.*s (%d pages)\n", v->width, v->title, d->pages);
  doc_free(d);
  view_free(v);
  return 0;
}
