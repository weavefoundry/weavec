// RFC 0037 §5: a format that is not a constant is read by fprintf's
// wrapper, which checks the strings its %s arguments name.
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *s = malloc(8);
  volatile const char *format = "%s\n";
  FILE *out = fopen("/dev/null", "w");
  memset(s, 'A', 8);
  return fprintf(out, (const char *)format, s); // TRAP: unterminated-string
}
