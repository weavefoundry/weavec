// RFC 0034 detection set, case 32 (double free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// UNITS: strbuf.c
// RUN-INPUT: one two three --flush four
#include "strbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct report {
  struct strbuf body;
  int lines;
};

static void report_cleanup(struct report *r) {
  free(r->body.data); // STOP
  r->lines = 0;
}

int main(int argc, char **argv) {
  struct report r = {0};
  if (strbuf_init(&r.body, 8))
    return 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--flush") == 0) {
      printf("%s\n", r.body.data ? r.body.data : "");
      strbuf_release(&r.body);
      break;
    }
    strbuf_add(&r.body, argv[i]);
    strbuf_add(&r.body, " ");
    r.lines++;
  }
  report_cleanup(&r);
  return 0;
}
