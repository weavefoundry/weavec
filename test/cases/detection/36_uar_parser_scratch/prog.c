// RFC 0034 detection set, case 36 (use after return): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 'alpha beta gamma'
#include <stdio.h>
#include <stdlib.h>

struct parser {
  const char *input;
  size_t pos;
  char *scratch;
#ifdef FIX
  char store[64];
#endif
};

static void parser_init(struct parser *p, const char *in) {
#ifdef FIX
  p->scratch = p->store;
#else
  char buf[64];
  p->scratch = buf; /* outlives this call */
#endif
  p->input = in;
  p->pos = 0;
}

static const char *parser_next(struct parser *p) {
  size_t i = 0;
  while (p->input[p->pos] == ' ')
    p->pos++;
  while (p->input[p->pos] && p->input[p->pos] != ' ' && i < 63)
    p->scratch[i++] = p->input[p->pos++]; // STOP // MISS: a use after return: a dead stack object is untracked (RFC 0032 limit); the trap here was incidental, and the program now traps at the printf of the token
  p->scratch[i] = '\0';
  return i ? p->scratch : NULL;
}

static void noise(int n) {
  volatile char pad[256];
  for (int i = 0; i < 256; i++)
    pad[i] = (char)(n + i);
  (void)pad[n & 255];
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct parser p;
  parser_init(&p, argv[1]);
  noise(argc);
  const char *tok;
  while ((tok = parser_next(&p)))
    printf("token: %s\n", tok);
  return 0;
}
