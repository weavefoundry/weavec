// RFC 0034 detection set, case 61 (invalid free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 501
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_anonymous[] = "anonymous";

struct user {
  char *name;
  int uid;
};

static void user_init(struct user *u, const char *name, int uid) {
#ifdef FIX
  u->name = strdup(name && *name ? name : g_anonymous);
#else
  u->name = name && *name ? strdup(name) : g_anonymous;
#endif
  u->uid = uid;
}

static void user_destroy(struct user *u) {
  free(u->name); // STOP
  u->name = NULL;
}

int main(int argc, char **argv) {
  struct user u;
  user_init(&u, argc > 2 ? argv[2] : "", atoi(argv[1]));
  printf("uid %d is %s\n", u.uid, u.name);
  user_destroy(&u);
  return 0;
}
