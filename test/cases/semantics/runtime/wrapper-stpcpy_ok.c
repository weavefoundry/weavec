// RFC 0034 §5.2: the correct twin of wrapper-stpcpy_bug.c: seven characters and their terminator fit the block.
// CLEAN
// RUN-INPUT: 0123456
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(char *dst, char **words, int i) {
  char *end = stpcpy(dst, words[i]); // GUARDED: spatial
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
