// RFC 0033 §5: the twin of format-global-array_bug.c; the global array holds
// a terminated string, so its `%s` check passes.
// STAGE: S8
// CLEAN
// RUN-INPUT: x
#include <stdio.h>
#include <string.h>
char name[8];
int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1)
    memset(name, 'x', sizeof name - 1);
  else
    strcpy(name, "ok");
  printf("%s\n", name);
  return 0;
}
