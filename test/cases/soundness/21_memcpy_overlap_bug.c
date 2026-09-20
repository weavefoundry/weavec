// memcpy with overlapping source and destination.
// ASAN
#include <string.h>
int main(void) {
  char buf[16] = "abcdefghijklmno";
  memcpy(buf + 2, buf, 8); // BUG: out-of-bounds
  return buf[3];
}
