// RFC 0034 §5.2: strcat appends an element of an array it was passed (no term names it) to a string in a block whose extent its function cannot see: its checked wrapper checks both lengths and the terminator against the room left in the block.
// RUN-INPUT: 012345
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(char *dst, char **words, int i) {
  dst[0] = '>';
  dst[1] = ' ';
  dst[2] = 0;
  strcat(dst, words[i]); // TRAP: object // GUARDED: spatial
}

int main(int argc, char **argv) {
  char *dst = malloc(8);
  if (!dst || argc < 2)
    return 1;
  put(dst, argv, 1);
  printf("%s\n", dst);
  free(dst);
  return 0;
}
