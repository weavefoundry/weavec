// RFC 0034 §5.2: the correct twin of wrapper-vsprintf_bug.c: the output and its terminator fit the block.
// CLEAN
// RUN-INPUT: 012
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int say(char *out, const char *format, ...) {
  va_list ap;
  int written;
  va_start(ap, format);
  written = vsprintf(out, format, ap); // GUARDED: spatial
  va_end(ap);
  return written;
}
static void put(char *dst, char **words, int i) { say(dst, "[%s]:%d", words[i], i); }

int main(int argc, char **argv) {
  char *dst = malloc(8);
  if (!dst || argc < 2)
    return 1;
  put(dst, argv, 1);
  printf("%s\n", dst);
  free(dst);
  return 0;
}
