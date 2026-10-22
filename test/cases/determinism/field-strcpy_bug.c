// RFC 0036 §6: a fortified strcpy into an 8-byte field followed by
// another stops at the call: it writes more than the field's object size.
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
  strcpy(u->name, argc > 1 ? argv[1] : "AAAAAAAAAAA"); // TRAP: intra-object-overflow
  printf("%d\n", u->admin);
  free(u);
  return 0;
}
