// RFC 0030 §7.4: a cast end sentinel '(struct rec *)(buf + len)' compiles without a trap.
// STAGE: S3
// Outside a required position a conversion carries no obligation (the Departure of §7.4
// item 4): 'end' points one past the records and is only compared. The obligation sits at
// the dereferences, which the loop keeps inside the array. No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>

struct rec { int a; int b; };

static int total(char *buf, size_t len) {
  struct rec *end = (struct rec *)(buf + len);
  int n = 0;
  for (struct rec *r = (struct rec *)buf; r < end; r++) n += r->a;
  return n;
}

int main(void) {
  struct rec recs[4] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
  char *buf = (char *)recs;
  size_t len = sizeof recs;
  struct rec *end = (struct rec *)(buf + len);
  int n = 0;
  for (struct rec *r = (struct rec *)buf; r < end; r++) n += r->a;
  return n == 10 && total(buf, len) == 10 ? 0 : 1;
}
