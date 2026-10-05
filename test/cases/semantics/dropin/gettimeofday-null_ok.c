// RFC 0033 §3: gettimeofday accepts a null time value on Darwin and glibc (redis's util.c
// asks only for the time zone).
// STAGE: S2
// CLEAN
// RUN-INPUT:
#include <stddef.h>
#include <sys/time.h>
int main(void) {
  struct timezone tz;
  struct timeval tv;
  if (gettimeofday(NULL, &tz) != 0) return 1;
  return gettimeofday(&tv, NULL) == 0 ? 0 : 1;
}
