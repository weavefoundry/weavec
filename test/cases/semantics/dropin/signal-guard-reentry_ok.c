// RFC 0033 §6.3: a guard in a signal handler that interrupted another guard's
// lookup of a global object finds it too (libuv's signal handler interrupted
// uv_fs_req_cleanup's guard and waited forever on the globals' lock).
// STAGE: S7
// CLEAN
// RUN-INPUT: 200000
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
static int table[64];
static int other[64];
static volatile sig_atomic_t ticks;
static int *volatile pick(int i) { return (i & 1) ? table : other; }
static void onTick(int signal) {
  (void)signal;
  int *slot = pick(ticks);
  slot[ticks & 63] += 1;
  ticks = ticks + 1;
}
int main(int argc, char **argv) {
  struct itimerval every;
  long rounds = argc > 1 ? atol(argv[1]) : 0;
  long sum = 0;
  every.it_interval.tv_sec = 0;
  every.it_interval.tv_usec = 50;
  every.it_value = every.it_interval;
  signal(SIGALRM, onTick);
  setitimer(ITIMER_REAL, &every, NULL);
  for (long i = 0; i < rounds * 100; ++i) {
    int *slot = pick((int)i);
    sum += slot[i & 63];
  }
  every.it_value.tv_usec = 0;
  every.it_interval.tv_usec = 0;
  setitimer(ITIMER_REAL, &every, NULL);
  return sum < 0;
}
