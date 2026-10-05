// RFC 0034 section 6.3 (shallow const): a variant of const-shallow-callback.c
// with the request in a local and the out-pointer two levels below it. The
// callee, in another unit, gets a pointer to a const request, but const is
// shallow: it writes the result through the pointers the request holds, so
// neither 'n' nor 'name' is uninitialised after the call.
// CLEAN
// UNITS: const-shallow-local-callee.c
#include <stdio.h>

struct out { int *count; const char **name; };
struct request { struct out *out; int id; };

int fill(const struct request *req); /* in another unit */

int main(void) {
  int n;
  const char *name;
  struct out out = {&n, &name};
  struct request req = {&out, 7};
  if (fill(&req) != 0)
    return 1;
  printf("%d %s\n", n, name);
  return 0;
}
