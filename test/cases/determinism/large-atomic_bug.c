// RFC 0037 §4: an atomic load of an object too large to be lock-free is a
// call of __atomic_load, whose row guards both objects: a source that ends
// early stops it.
// UNITS: Inputs/atomic-load.c
// RUN-INPUT: 1
#include <stdlib.h>

struct big { char bytes[32]; };

int main(int argc, char **argv) {
  struct big out;
  struct big *in = malloc(argc > 1 ? 16 : sizeof(struct big));
  __atomic_load(in, &out, __ATOMIC_SEQ_CST); // TRAP: heap-buffer-overflow
  free(in);
  return out.bytes[0];
}
