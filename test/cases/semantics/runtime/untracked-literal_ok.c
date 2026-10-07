// RFC 0032 (A6): memory the object table does not track passes a guard: a string literal, 'argv', a block of another allocator.
// STAGE: S3
// CLEAN
// RUN-INPUT: word
// ASAN
#include <stdio.h>
struct text { const char *p; };
static int third(const struct text *t) {
  return t->p[2];
}
int main(int argc, char **argv) {
  struct text lit = {"literal"};
  struct text arg = {argc > 1 ? argv[1] : "none"};
  return (third(&lit) != 't') + (third(&arg) != 'r');
}
