// Length truncated to 8 bits passes the check; full length is copied.
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
// ASAN
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  char buf[64];
  size_t len = strlen(argv[1]);
  unsigned char n = (unsigned char)len;
  if (n < sizeof buf) memcpy(buf, argv[1], len); // BUG: out-of-bounds // TRAP: len
  return buf[0];
}
