// RFC 0036 §6: a fortified strcpy into an 8-byte field followed by
// another stops at the call: it writes more than the field's object size.
// The call is spelled as Darwin's headers fortify `strcpy` at the call site
// (the field's size, 8); glibc's inline wrappers pass the rest of the
// object instead (amendment 9), so the case spells it out on every platform.
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
  __builtin___strcpy_chk(u->name, argc > 1 ? argv[1] : "AAAAAAAAAAA", __builtin_object_size(u->name, 1)); // TRAP: intra-object-overflow
  printf("%d\n", u->admin);
  free(u);
  return 0;
}
