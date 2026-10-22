// RFC 0036 §6: a fortified string call writes no more than the object
// size its caller passes (here the 8-byte field, as Darwin's headers pass
// it at the call site; glibc's inline wrappers pass the rest of the object,
// amendment 9, so the test spells the call out); a write past it within the
// struct stops.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t short
// RUN: not --crash %t AAAAAAAAAAA 2>&1 | FileCheck %s
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
  // CHECK: weavec: intra-object-overflow at {{.*}}intra-object.c:[[@LINE+2]]:{{[0-9]+}}: write of 12 bytes at 0x[[AT:[0-9a-f]+]]
  // CHECK-NEXT: weavec: 0x[[AT]] is 0 bytes inside the 8-byte destination at 0x[[AT]]
  __builtin___strcpy_chk(u->name, argv[argc - 1],
                         __builtin_object_size(u->name, 1));
  return u->admin;
}
