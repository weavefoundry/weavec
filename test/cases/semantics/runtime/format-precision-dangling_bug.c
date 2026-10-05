// RFC 0033 §5, RFC 0034 amendments: a `%.Ns` argument is read for at most N
// bytes and needs no terminator, so it states no spatial need; it is still
// read, so the call's liveness guard covers it. Here it points into a copy
// the callee freed.
// RUN-INPUT:
// ASAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void find(const char *text, const char **err) {
  char *copy = strdup(text);
  *err = text;
  for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ","))
    if (tok[0] == 'x') {
      *err = tok;
      break;
    }
  free(copy);
}
int main(void) {
  const char *err;
  find("a,b,xyz", &err);
  printf("%.3s\n", err); // BUG: use-after-free // TRAP: live
  return 0;
}
