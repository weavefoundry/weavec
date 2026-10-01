// A conditional whose arm is in parentheses (`c ? (x = i, 5) : 0`, as in
// Lua's `tonumberns`) records its value under the operator on that path too:
// the CFG evaluates the expression inside the parentheses, and the value an
// earlier iteration's other arm left (0) is not the operator's value here.
// ASAN
// RUN-INPUT: 3
#include <stdlib.h>

static int pick(int n) {
  int a[4] = {0};
  int x = 0;
  int total = 0;
  for (int i = 0; i < n; i++) {
    int t = (i & 1) ? (x = i, 5) : 0;
    total += a[t]; // BUG: out-of-bounds // TRAP: index // NOT-PROVEN: spatial
  }
  return total + x;
}

int main(int argc, char **argv) {
  return pick(argc > 1 ? atoi(argv[1]) : 0);
}
