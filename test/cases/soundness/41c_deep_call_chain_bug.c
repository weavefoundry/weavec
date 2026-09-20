// UAF where the free happens six calls deep.
// ASAN
#include <stdlib.h>
static void f6(char *p) { free(p); }
static void f5(char *p) { f6(p); }
static void f4(char *p) { f5(p); }
static void f3(char *p) { f4(p); }
static void f2(char *p) { f3(p); }
static void f1(char *p) { f2(p); }
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  f1(p);
  return p[0]; // BUG: use-after-free
}
