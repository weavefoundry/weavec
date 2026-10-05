// RFC 0034 section 6.3: a false definite mismatched release the milestone
// found in Janet's net.c (build/eval-2026-10-04/repros/janet-1.c): the
// function returns either calloc'd memory (*is_unix = 1) or a getaddrinfo
// list, and the caller releases each with its own releaser by the flag. An
// unconfirmed candidate may remain a warning (RFC 0034, Diagnostics).
// CLEAN
// ALLOW: mismatched-release
// RUN-INPUT:
// RUN-INPUT: unix
/* Janet net.c: janet_get_addrinfo returns either a calloc'd sockaddr_un
 * (and sets *is_unix = 1) or a getaddrinfo list (*is_unix = 0). Callers
 * call freeaddrinfo only when !is_unix. weavec-cc reports a definite
 * mismatched-release error and fails the build. */
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

static struct addrinfo *get_ai(const char *host, int *is_unix) {
#ifndef NEGATIVE
  if (strcmp(host, "unix") == 0) {
    struct sockaddr_un *sa = calloc(1, sizeof *sa);
    if (!sa) abort();
    sa->sun_family = AF_UNIX;
    *is_unix = 1;
    return (struct addrinfo *)sa;
  }
#endif
  struct addrinfo hints, *ai = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  if (getaddrinfo(host, "80", &hints, &ai)) abort();
  *is_unix = 0;
  return ai;
}

int main(int argc, char **argv) {
  int is_unix = 0;
  struct addrinfo *ai = get_ai(argc > 1 ? argv[1] : "127.0.0.1", &is_unix);
  if (is_unix) { free(ai); puts("unix"); return 0; }
  printf("family %d\n", ai->ai_family);
  freeaddrinfo(ai);
  return 0;
}
