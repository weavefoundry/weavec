// RFC 0034 section 6.3: a false definite use of an uninitialised value the
// milestone found in libgit2's tests/libgit2/online/clone.c
// (build/eval-2026-10-04/repros/libgit2-2.c): run_with_callbacks takes a
// pointer to a const struct, but C's const is shallow, and the callback it
// calls writes given_url through the payload pointer stored in the struct.
// The callee is in another unit. An unconfirmed candidate may remain a
// warning (RFC 0034, Diagnostics).
// CLEAN
// ALLOW: use-of-uninitialized
// UNITS: const-shallow-callback-callee.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct callbacks { int (*cred)(void *payload); void *payload; };
struct options { struct callbacks cb; int other; };

static struct options g_options;
int run_with_callbacks(const struct options *opts); /* in another unit */

static int cred(void *payload) {
  *(char **)payload = strdup("http:// example.com/");
  return 0;
}

int main(void) {
  char *given_url;
  g_options.cb.cred = cred;
  g_options.cb.payload = &given_url;
  if (run_with_callbacks(&g_options) != 0)
    return 1;
  printf("%s\n", given_url);
  free(given_url);
  return 0;
}
