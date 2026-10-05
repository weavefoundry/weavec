// RFC 0033 §1: passing a released pointer to a function that only compares, hashes or
// prints its address (a table keyed by address, redis's xmalloc debug table) accesses no
// memory: no use-after-free.
// STAGE: S7
// CLEAN
// RUN-INPUT:
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define SLOTS 8
static void *keys[SLOTS];
static unsigned slot_of(void *key) { return (unsigned)((uintptr_t)key >> 4) % SLOTS; }
static void forget(void *key) {
  unsigned i = slot_of(key);
  if (keys[i] != key) {
    fprintf(stderr, "unknown key %p\n", key);
    return;
  }
  keys[i] = NULL;
}
int main(void) {
  char *p = malloc(16);
  if (p == NULL) return 1;
  keys[slot_of(p)] = p;
  free(p);
  forget(p);
  return 0;
}
