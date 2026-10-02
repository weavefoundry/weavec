// RFC 0030 §3.1: a callee that may release an argument already freed is a
// second release.
// STAGE: S7
// `release_line` frees its argument unless it is the sentinel (linenoise's
// `linenoiseFree`); the caller freed the line already, so the call frees it
// twice. The callee's release is only possible, but the argument is freed for
// certain: the finding is the double free, not a use of a freed pointer.
// TOOL
#include <stdlib.h>

char *sentinel_value = "more";

void release_line(void *line) {
  if (line == sentinel_value)
    return;
  free(line);
}

char *read_line(void);

int main(void) {
  char *line = read_line();
  if (line == NULL)
    return 0;
  free(line);
  release_line(line); // BUG: double-free definite
  return 0;
}
