// RFC 0034 §5.2: stpcpy copies an element of an array it was passed (no term names it) into a block whose extent its function cannot see: its checked wrapper checks strlen + 1 bytes against the room left in the block.
// RUN-INPUT: 0123456789abcdef
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(char *dst, char **words, int i) {
  char *end = stpcpy(dst, words[i]); // TRAP
  end[0] = 0;
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
