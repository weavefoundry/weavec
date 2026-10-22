// The twin of global-jump_bug.c, its index inside `name`.
// CLEAN
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

char name[32] = "public";
char secret[32] = "topsecret";

int main(int argc, char **argv) {
  char *p = name;
  long i = argc > 1 ? atol(argv[1]) : 2;
  printf("%c %c\n", p[i], secret[0]);
  return 0;
}
