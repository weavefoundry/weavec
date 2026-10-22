// RFC 0036 §2: locals in the disjoint arms of a switch share the frame's
// bytes, so a recursion through them reaches the depth the reference
// compiler's frames reach (10,000 frames here, where v0.16.0's frames, one
// redzoned slot per local, overflowed an 8 MiB stack before 7,000).
// CLEAN
// FLAGS: -O2
// RUN-INPUT: 10000
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static void use(char *p, int n) { p[n] = 1; }

__attribute__((noinline)) static int depth(int n, int kind) {
  if (n == 0)
    return 0;
  switch (kind & 3) {
  case 0: { char a[200]; use(a, n & 7); return depth(n - 1, kind + 1) + a[n & 7]; }
  case 1: { char b[200]; use(b, n & 7); return depth(n - 1, kind + 1) + b[n & 7]; }
  case 2: { char c[200]; use(c, n & 7); return depth(n - 1, kind + 1) + c[n & 7]; }
  default: { char d[200]; use(d, n & 7); return depth(n - 1, kind + 1) + d[n & 7]; }
  }
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1000;
  printf("%d\n", depth(n, 0));
  return 0;
}
