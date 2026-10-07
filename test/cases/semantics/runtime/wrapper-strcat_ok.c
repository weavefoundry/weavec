// RFC 0034 §5.2: the correct twin of wrapper-strcat_bug.c: two characters, five more and the terminator fit the block.
// CLEAN
// RUN-INPUT: 01234
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(char *dst, char **words, int i) {
  dst[0] = '>';
  dst[1] = ' ';
  dst[2] = 0;
  strcat(dst, words[i]);
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
