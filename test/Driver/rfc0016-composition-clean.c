// RFC 0016: serialized contexts preserve safe ordering and output replacement.
// RUN: split-file %s %t
// RUN: %weavec_cc -I%S/../Inputs -c %t/callee.c -o %t/callee.o
// RUN: %weavec_cc -I%S/../Inputs -c %t/caller.c -o %t/caller.o
// RUN: %weavec_cc %t/callee.o %t/caller.o -o %t/program
// RUN: %t/program

//--- callee.c
#include "prelude.h"
void before(char *a, char *b) { *b = 1; free(a); }
void reset(char **a, char **b) { free(*a); *a = malloc(4); if (*b) **b = 1; }
//--- caller.c
#include "prelude.h"
void before(char *, char *);
void reset(char **, char **);
int main(void) {
  char *p = malloc(4); if (!p) return 0;
  before(p, p);
  p = malloc(4); if (!p) return 0;
  reset(&p, &p);
  if (p) *p = 2;
  free(p);
  return 0;
}
