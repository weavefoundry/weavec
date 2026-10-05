// RFC 0034 detection set, case 17 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 'supercalifragilistic expialidocious'
#include <stdio.h>

static void copy_word(char *dst, const char *src) {
#ifdef FIX
  char *end = dst + 7;
  while (*src && *src != ' ' && dst < end)
    *dst++ = *src++;
#else
  while (*src && *src != ' ')
    *dst++ = *src++; // STOP
#endif
  *dst = '\0';
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  char word[8];
  int count = argc;
  copy_word(word, argv[1]);
  printf("first word: %s (%d)\n", word, count);
  return 0;
}
