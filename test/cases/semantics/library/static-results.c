// RFC 0030 §8.3: getenv, strtok, localtime and strerror return static storage with the right lifetime.
// STAGE: S4
// Their rows give static(S) or interior-state(S) results: borrows of a hidden state slot,
// never fresh objects. Not freeing them is no leak, and each borrow is used before the next
// call that reads or invalidates its slot. No error, no warning, no trap.
// CLEAN
// ASAN
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void) {
  char line[] = "a,b,c";
  int fields = 0;
  for (char *tok = strtok(line, ","); tok != NULL; tok = strtok(NULL, ","))
    fields += tok[0] != 0;
  const char *home = getenv("HOME");
  size_t length = home ? strlen(home) : 0;
  time_t t = 0;
  struct tm *tm = localtime(&t);
  int year = tm ? tm->tm_year : 70;
  const char *msg = strerror(2);
  return fields == 3 && msg[0] != 0 && year >= 69 && length < 4096 ? 0 : 1;
}
