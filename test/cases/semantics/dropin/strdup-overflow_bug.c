// RFC 0033 §6.2: a write past a strdup'd string's block traps (on Darwin the block came from
// the system's zone and was untracked). The helper writes only when the environment asks
// for it, so no caller contract covers the write and only the runtime knows the extent.
// STAGE: S4
// RUN-INPUT: 32
// ASAN
#include <stdlib.h>
#include <string.h>
static void scrub(char *p, int n) {
  if (getenv("WEAVEC_CASE_KEEP") != NULL) return;
  p[n] = '*'; // BUG: out-of-bounds // TRAP
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char *secret = strdup("hunter2");
  if (secret == NULL) return 1;
  scrub(secret, atoi(argv[1]));
  int r = secret[0];
  free(secret);
  return r == '*' ? 0 : 1;
}
