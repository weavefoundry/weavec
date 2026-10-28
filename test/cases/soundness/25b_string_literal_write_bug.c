// Write into a string literal via strtok: the literal is read-only, so the
// write faults.
#include <string.h>
int main(void) {
  char *s = "a,b";
  char *t = strtok(s, ","); // TRAP
  return t ? t[0] : 0;
}
