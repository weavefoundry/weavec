// Root-cause repro mainleak: an early return from main leaves 'b' allocated; v0.10.0
// reports a leak (benign at exit).
// intended (RFC 0030 section 8.4, gate S4): leaks are not reported at a return from main.
// CLEAN
// RUN-INPUT:
// RUN-INPUT: < Inputs/mainleak-x.txt
// ASAN
#include <stdlib.h>
#include <stdio.h>
int main(void) { char *b = malloc(10); if (!b) return 1; if (getchar() == 'x') return 2; free(b); return 0; }
