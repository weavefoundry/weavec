// Stack overflow: strcpy / sprintf of argv into fixed buffer.
// The destination's requirement is strlen(argv[1]) + 1 bytes against the eight
// of 'buf', which is checkable only as __weavec_strnlen(argv[1], 8) + 1 — and
// §10.3 rule 1 lets a check term name a constant, a `sizeof`, a parameter, a
// local, or a field reached from one through `.` and `->`, never a subscript,
// so 'argv[1]' has no spelling the term language holds, and no `len` check
// is planned. RFC 0034 section 5.2: the call becomes strcpy's checked
// wrapper, which computes strlen(argv[1]) + 1 at run time and checks it
// against the object 'buf' (registered, since a guard reaches it), so the
// facet is guarded and the copy traps before it writes. ASan sees the same
// overflow.
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
// ASAN
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 0;
  strcpy(buf, argv[1]); // TRAP
  return buf[0];
}
