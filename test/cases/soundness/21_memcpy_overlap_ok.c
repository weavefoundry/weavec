// CLEAN
// ASAN
#include <string.h>
int main(void) {
  char buf[16] = "abcdefghijklmno";
  memmove(buf + 2, buf, 8);
  return buf[3];
}
