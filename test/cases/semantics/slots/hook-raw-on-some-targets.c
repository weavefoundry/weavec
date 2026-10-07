// RFC 0031 *Implementation amendments*: raw through some of a hook's
// functions.
// STAGE: S7
// `allocate` holds `calloc` or, while a test installs it, `bogus`, which
// returns an integer as a pointer (hiredis's allocator injection test). The
// call's result is raw through one of its functions only: using it is no
// definite `unsafe-operation` (each such site was one, in every function
// that allocates), and nothing about it is proven. A raw value a local join
// makes stays an error (`test/Analysis/rfc0004-raw.c`, "Rawness joins").
// CLEAN
// RUN-INPUT:
#include <stdint.h>
#include <stdlib.h>

struct fns {
  void *(*allocate)(size_t, size_t);
};

static void *bogus(size_t count, size_t size) {
  (void)count;
  (void)size;
  return (void *)(uintptr_t)0xdeadc0de;
}

static struct fns hooks = {calloc};

struct reply {
  int type;
  char *str;
};

static struct reply *make(int type) {
  struct reply *r = hooks.allocate(1, sizeof *r);
  if (r == NULL)
    return NULL;
  r->type = type;
  return r;
}

int main(int argc, char **argv) {
  (void)argv;
  if (argc > 5) {
    hooks.allocate = bogus;
    hooks.allocate = calloc;
  }
  struct reply *r = make(1);
  if (r == NULL)
    return 1;
  int type = r->type;
  free(r);
  return type == 1 ? 0 : 1;
}
