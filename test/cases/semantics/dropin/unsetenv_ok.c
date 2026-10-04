// RFC 0033 §6: the runtime's free reads no environment variable. The C
// library frees with its environment lock held (`unsetenv`), and `getenv`
// takes the lock again (libuv's env_vars test was killed there).
// STAGE: S7
// CLEAN
// RUN-INPUT: 3
#include <stdlib.h>
int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 0;
  for (int i = 0; i < rounds; ++i) {
    if (setenv("WEAVEC_CASE_VARIABLE", "123456789", 1) != 0) return 1;
    if (setenv("WEAVEC_CASE_VARIABLE", "", 1) != 0) return 1;
    if (unsetenv("WEAVEC_CASE_VARIABLE") != 0) return 1;
  }
  return getenv("WEAVEC_CASE_VARIABLE") != NULL;
}
