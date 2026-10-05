// RFC 0033 §3: setgroups(0, NULL) drops every supplementary group; a count of zero allows a
// null list in every supported C library (libuv's process.c).
// STAGE: S2
// CLEAN
// RUN-INPUT:
#include <grp.h>
#include <unistd.h>
int main(void) {
  /* Fails with EPERM unless run as root, which is fine: the call is what matters. */
  int r = setgroups(0, NULL);
  return r == 0 || r == -1 ? 0 : 1;
}
