// RFC 0034 detection set, case 35 (invalid free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: bob a_much_longer_name_that_needs_the_heap
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* formats into the caller's scratch buffer when it fits, else allocates */
static char *render(const char *name, char *scratch, size_t scratch_len) {
  size_t need = strlen(name) + sizeof "Hello, !";
  char *out = need <= scratch_len ? scratch : malloc(need);
  if (!out)
    return NULL;
  snprintf(out, need, "Hello, %s!", name);
  return out;
}

int main(int argc, char **argv) {
  char small[32];
  for (int i = 1; i < argc; i++) {
    char *msg = render(argv[i], small, sizeof small);
    if (!msg)
      return 1;
    puts(msg);
#ifdef FIX
    if (msg != small)
#endif
      free(msg); // STOP
  }
  return 0;
}
