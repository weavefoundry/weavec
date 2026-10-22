// RFC 0036 §5: the twin of asan-pool_bug.c, which copies only its object.
// CLEAN
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#endif
#endif
#ifndef POISON
#define POISON(p, n) ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static char *pool;
static size_t top;

static void *palloc(size_t n) {
  void *p = pool + top;
  top += (n + 15) & ~(size_t)15;
  UNPOISON(p, n);
  return p;
}

int main(void) {
  pool = malloc(4096);
  if (pool == NULL)
    return 1;
  POISON(pool, 4096);
  char *a = palloc(16);
  char *b = palloc(16);
  memset(a, 'a', 16);
  memset(b, 'b', 16);
  char out[32];
  memcpy(out, a, 16);
  memcpy(out + 16, b, 16);
  printf("%c%c\n", out[0], out[16]);
  free(pool);
  return 0;
}
