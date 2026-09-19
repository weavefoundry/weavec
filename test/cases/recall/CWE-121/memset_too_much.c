// CWE-121: stack-based buffer overflow through memset of the wrong size.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

struct record {
  int id;
  char name[16];
};

void bad(void) {
  char buf[32];
  memset(buf, 0, 64); // BUG: out-of-bounds // TRAP: len
  print_bytes(buf, 32);
}

void bad_member(struct record *r) {
  r->name[16] = 0; // BUG: out-of-bounds // TRAP: index
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  struct record r;
  if (which == 1) bad();
  if (which == 2) bad_member(&r);
  return 0;
}
