// RFC 0034 detection set, case 13 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: GET /api/v1/users/12345/profile/settings
#include <stdio.h>
#include <string.h>

struct request {
  const char *method;
  const char *path;
};

static void handle(const struct request *r) {
  char route[16];
#ifdef FIX
  snprintf(route, sizeof route, "%s", r->path);
#else
  strcpy(route, r->path); // STOP
#endif
  for (char *p = route; *p; p++)
    if (*p == '/')
      *p = '_';
  printf("%s -> handler%s\n", r->method, route);
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct request r = {argv[1], argv[2]};
  handle(&r);
  return 0;
}
