// RFC 0034 detection set, case 15 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: administrat
#include <stdio.h>
#include <string.h>

struct account {
  char user[8];
  int is_admin;
  int balance;
};

static void login(struct account *a, const char *user) {
  a->is_admin = 0;
  a->balance = 100;
#ifdef FIX
  snprintf(a->user, sizeof a->user, "%s", user);
#else
  strcpy(a->user, user); // STOP
#endif
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct account acct;
  login(&acct, argv[1]);
  printf("user=%.8s admin=%s\n", acct.user, acct.is_admin ? "yes" : "no");
  return 0;
}
