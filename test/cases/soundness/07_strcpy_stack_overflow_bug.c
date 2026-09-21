// Stack overflow: strcpy / sprintf of argv into fixed buffer.
// The destination's requirement is strlen(argv[1]) + 1 bytes against the eight
// of 'buf', which is checkable only as __weavec_strnlen(argv[1], 8) + 1 — and
// §10.3 rule 1 lets a check term name a constant, a `sizeof`, a parameter, a
// local, or a field reached from one through `.` and `->`, never a subscript,
// so 'argv[1]' has no spelling the term language holds. The site therefore
// stays unresolved(inexpressible) and no `len` check is planned. Hoisting the
// source into a temporary (`char *s = argv[1];`) is checked and traps, so the
// gap is the term language, not the length check; widening rule 1 needs an
// amendment to RFC 0030. ASan sees the real overflow, and the spatial facet
// of the call is unresolved, so nothing is proven of it. The trap-mode run
// still dies, on macOS by _FORTIFY_SOURCE's own check, which the runner notes
// as the program's own trap.
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
// ASAN
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 0;
  strcpy(buf, argv[1]); // MISS: the length term would name argv[1], which §10.3 rule 1 has no spelling for
  return buf[0];
}
