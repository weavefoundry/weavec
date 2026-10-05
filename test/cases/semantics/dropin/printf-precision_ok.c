// RFC 0033 §5: a `%.Ns` argument is read for at most N bytes and needs no terminator; neither
// the analysis nor a guard asks for one.
// STAGE: S3
// CLEAN
// RUN-INPUT: 3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void show(const char *tag, int n) {
  printf("[%.3s] [%.*s]\n", tag, n, tag);
}
int main(int argc, char **argv) {
  char *tag = malloc(3);
  char fixed[3];
  if (tag == NULL || argc < 2) return 1;
  memcpy(tag, "abc", 3);
  memcpy(fixed, "xyz", 3);
  show(tag, atoi(argv[1]));
  printf("%.3s\n", fixed);
  free(tag);
  return 0;
}
