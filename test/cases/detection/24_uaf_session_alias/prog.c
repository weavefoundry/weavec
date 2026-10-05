// RFC 0034 detection set, case 24 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 103 alice bob carol
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct user {
  char name[32];
  int id;
};

struct session {
  struct user *user;
  int token;
};

struct registry {
  struct user *users[8];
  int n;
};

static struct user *registry_add(struct registry *r, const char *name, int id) {
  if (r->n == 8)
    return NULL;
  struct user *u = calloc(1, sizeof *u);
  if (!u)
    return NULL;
  snprintf(u->name, sizeof u->name, "%s", name);
  u->id = id;
  r->users[r->n++] = u;
  return u;
}

static void registry_remove(struct registry *r, int id) {
  for (int i = 0; i < r->n; i++) {
    if (r->users[i]->id == id) {
      free(r->users[i]);
      r->users[i] = r->users[--r->n];
      return;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct registry reg = {0};
  for (int i = 2; i < argc; i++)
    registry_add(&reg, argv[i], 100 + i);
  struct session s = {reg.users[1], 4242};
  int victim = atoi(argv[1]);
#ifdef FIX
  if (s.user && s.user->id == victim)
    s.user = NULL; /* invalidate the session before the user goes */
#endif
  registry_remove(&reg, victim);
#ifdef FIX
  if (s.user)
#endif
    printf("session %d belongs to %s\n", s.token, s.user->name); // STOP
  for (int i = 0; i < reg.n; i++)
    free(reg.users[i]);
  return 0;
}
