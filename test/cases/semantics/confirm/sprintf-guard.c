// RFC 0034 section 5.2 (checked wrappers) and Silent misses: the sprintf
// guard gap of the milestone's blind detection set
// (build/eval-2026-10-04/detect/findings/sprintf_guard_gap). fmt() writes
// 37 bytes into a 16-byte heap block whose extent its unit cannot see, and
// nothing reads the block afterwards. A guard of the format's least output
// did not bound what sprintf writes; its checked wrapper writes through
// vsnprintf with the room left in the block. The default build must trap at
// the sprintf; -DFIX allocates enough.
// DETECT: -DFIX
// FLAGS: -O2
// UNITS: sprintf-guard-fmt.c
// RUN-INPUT: s averyveryverylonghostname.example
#include <stdio.h>
#include <stdlib.h>
void fmt(char *out, const char *host, int port);
void fmt2(char *out, const char *host);
int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
#ifdef FIX
  char *heap = malloc(64);
#else
  char *heap = malloc(16);
#endif
  if (!heap)
    return 1;
  if (argv[1][0] == 's') fmt(heap, argv[2], 80); else fmt2(heap, argv[2]);
  printf("done %d\n", argc);   /* no read of heap afterwards */
  free(heap);
  return 0;
}
