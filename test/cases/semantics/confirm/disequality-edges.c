// RFC 0034 section 6.2 (disequalities): variants of unreachable-after-assert.c.
// Each function leaves a test's true edge for one constant early, so the null
// dereference under the same test later is on no path: a switch's default
// edge excludes its labels, a constant on the left of `==` refines the value
// on the right, and a join keeps the constants both sides exclude. The zone
// cannot hold any of these (`t` is in [0, 7] throughout).
// CLEAN
#include <stdio.h>

typedef struct B { int allowed; } B;

static int bySwitch(int flags, B *cur) {
  int t = flags & 7;
  B *b = NULL;
  switch (t) {
  case 1:
  case 2:
    return cur->allowed;
  default:
    break;
  }
  printf("switch %d\n", t);
  if (t == 2)
    return b->allowed;
  return 0;
}

static int byConstantLeft(int flags, B *cur) {
  int t = flags & 7;
  if (3 == t)
    return cur->allowed;
  B *b = NULL;
  printf("left %d\n", t);
  if (t == 3)
    return b->allowed;
  return 0;
}

static int acrossAJoin(int flags, B *cur, int w) {
  int t = flags & 7;
  if (t != 5) {
    B *b = NULL;
    if (w)
      puts("w");
    else
      puts("not w");
    if (t == 5)
      return b->allowed;
    return 0;
  }
  return cur->allowed;
}

int main(int argc, char **argv) {
  (void)argv;
  B one = {1};
  return bySwitch(argc - 1, &one) + byConstantLeft(argc - 1, &one) +
         acrossAJoin(argc - 1, &one, argc);
}
