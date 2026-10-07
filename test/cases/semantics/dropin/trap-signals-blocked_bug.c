// RFC 0033 §6.1: a failed check terminates the program even when it has blocked SIGTRAP and
// SIGILL (libuv's tests block every signal in some threads).
// STAGE: S4
// RUN-INPUT: 8
#include <signal.h>
#include <stdlib.h>
static int at(int *p, int i) {
  return p[i]; // BUG: out-of-bounds // TRAP
}
int main(int argc, char **argv) {
  sigset_t all;
  if (argc < 2) return 2;
  sigfillset(&all);
  sigprocmask(SIG_BLOCK, &all, NULL);
  int *v = calloc(4, sizeof(int));
  if (v == NULL) return 1;
  return at(v, atoi(argv[1]));
}
