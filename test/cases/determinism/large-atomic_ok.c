// The twin of large-atomic_bug.c: a source of the object's size.
// RUN-INPUT:
// CLEAN
#include <stdlib.h>

struct big { char bytes[32]; };

int main(void) {
  struct big out;
  struct big *in = calloc(1, sizeof(struct big));
  __atomic_load(in, &out, __ATOMIC_SEQ_CST);
  free(in);
  return out.bytes[0];
}
