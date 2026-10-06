// RFC 0034 detection set, case 46 (unterminated string): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: content-type:text/html host:example.org
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct field {
  char *name;
  size_t len;
};

/* splits "name:value"; the name is copied out of the input */
static int field_parse(struct field *f, const char *spec) {
  const char *colon = strchr(spec, ':');
  f->len = colon ? (size_t)(colon - spec) : strlen(spec);
#ifdef FIX
  f->name = malloc(f->len + 1);
#else
  f->name = malloc(f->len);
#endif
  if (!f->name)
    return -1;
  memcpy(f->name, spec, f->len);
#ifdef FIX
  f->name[f->len] = '\0';
#endif
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    struct field f;
    if (field_parse(&f, argv[i]))
      return 1;
    printf("field: %s\n", f.name); // STOP
    free(f.name);
  }
  return 0;
}
