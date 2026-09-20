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

// RFC 0030 §7.4: `name` is the trailing member, which -fstrict-flex-arrays=0
// makes flexible whatever its bound, so `r->name[16]` is only past the end of
// an object of 20 bytes. The definite shortfall is the call's: `&r` has 20
// bytes and `bad_member` needs 21 (§3.3, as for the soundness probes whose
// argument breaks the callee's requirement).
void bad_member(struct record *r) {
  r->name[16] = 0;
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  struct record r;
  if (which == 1) bad();
  if (which == 2) bad_member(&r); // BUG: out-of-bounds // TRAP: violation
  return 0;
}
