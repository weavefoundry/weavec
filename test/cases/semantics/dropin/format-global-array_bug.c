// RFC 0033 §5: a `%s` argument that is a global array is checked like a local
// one, its `strnlen` bounded by the array (bzip2's `inName`); a global array
// stands for its address in a check term.
// STAGE: S8
// RUN-INPUT: x
#include <stdio.h>
#include <string.h>
char name[8];
int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1)
    memset(name, 'x', sizeof name);
  else
    strcpy(name, "ok");
  printf("%s\n", name); // TRAP
  return 0;
}
