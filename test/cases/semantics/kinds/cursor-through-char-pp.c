// RFC 0030 §7.3: a cursor stored through a 'char **' demotes the slots it may reach.
// STAGE: S6
// The slot 'scanner.pos' only receives an allocation, which is Single-valid, but its address
// is passed to 'advance', which stores a cursor through a char ** lvalue. That demotes every
// address-taken pointer slot, 'scanner.pos' included, to Unknown, so s->pos[0] in 'peekc'
// is unresolved(unknown-extent) instead of proven by a Single slot kind. The run advances
// the cursor one past the end; ASan reports the read.
// RUN-INPUT:
// ASAN
#include <stdlib.h>
#include <string.h>

struct scanner { char *pos; };

static void advance(char **pp, int k) { *pp += k; }

int peekc(struct scanner *s) { return s->pos[0]; } // BUG: out-of-bounds // UNRESOLVED: spatial:unknown-extent

int main(int argc, char **argv) {
  struct scanner s;
  (void)argv;
  s.pos = malloc(4);
  if (s.pos == NULL) return 1;
  memcpy(s.pos, "abc", 4);
  char *base = s.pos;
  advance(&s.pos, argc + 3);
  int c = peekc(&s);
  free(base);
  return c;
}
