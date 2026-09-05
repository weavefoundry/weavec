// CWE-121: stack-based buffer overflow through memset of the wrong size.
#include "recall.h"

struct record {
  int id;
  char name[16];
};

void bad(void) {
  char buf[32];
  memset(buf, 0, 64); // RECALL: out-of-bounds
  print_bytes(buf, 32);
}

void bad_member(struct record *r) {
  r->name[16] = 0; // RECALL: out-of-bounds
}
