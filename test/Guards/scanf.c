// RFC 0035 §2.5: a scanf-family call is checked after it returns, over what
// each conversion that ran wrote: a destination whose conversion did not
// run is no error, and one that ran past its object stops the program.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t "rgb 1 2 3" abc | FileCheck %s --check-prefix=SHORT
// RUN: not --crash %t "rgb 1 2 3" a-very-long-word 2>&1 | FileCheck %s --check-prefix=LONG
#include <stdio.h>

int main(int argc, char **argv) {
  int red = 0, green = 0, blue = 0;
  char keys[2];
  char *key = keys;
  char word[4];
  (void)argc;
  // Only three conversions run on a line without " is ": key + 40 is not
  // written, as the program's own check of the count relies on.
  // SHORT: 3
  printf("%d\n", sscanf(argv[1], "rgb %d %d %d is %c", &red, &green, &blue,
                        key + 40));
  // LONG: weavec: stack-buffer-overflow at {{.*}}scanf.c:[[@LINE+1]]:{{[0-9]+}}: write of
  return sscanf(argv[2], "%s", word) == 1 ? 0 : 1;
}
