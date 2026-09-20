// Write into a string literal via strtok.
// ASAN
#include <string.h>
int main(void) {
  char *s = "a,b";
  char *t = strtok(s, ","); // BUG: out-of-bounds
  return t ? t[0] : 0;
}
