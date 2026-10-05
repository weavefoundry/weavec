// RFC 0033 §6.2: memory the C library allocates (strdup) is tracked, so a use after its
// release traps (on Darwin it came from the system's zone and was untracked).
// STAGE: S4
// RUN-INPUT: 1
// ASAN
#include <stdlib.h>
#include <string.h>
static void drop(char *p, int really) { if (really) free(p); }
static char read_at(const char *p, int i) {
  return p[i]; // TRAP: object
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char *path = strdup("/tmp/weavec");
  if (path == NULL) return 1;
  drop(path, atoi(argv[1]));
  return read_at(path, 1); // BUG: use-after-free possible
}
