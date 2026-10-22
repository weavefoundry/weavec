// RFC 0036 §6: the twin of field-strcpy_bug.c, whose name fits.
// CLEAN
// FLAGS: -O2
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct user {
  char name[8];
  int admin;
};

int main(int argc, char **argv) {
  struct user *u = calloc(1, sizeof *u);
  if (u == NULL)
    return 1;
  strcpy(u->name, argc > 1 ? argv[1] : "AAAAAAA");
  printf("%s %d\n", u->name, u->admin);
  free(u);
  return 0;
}
