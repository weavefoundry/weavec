// RFC 0030 §4 "Checked, library length": strcpy into a fixed buffer checks the copy's length.
// STAGE: S6
// strcpy is a LibCall site with two spatial requirements. The destination's
// strlen(name) + 1 <= 16 is checked with the len template against the exact extent of 'buf'
// (the length comes from the bounded __weavec_strnlen, which reads at most 16 bytes). The
// source's NUL termination is inferred by R4 (§7.5) and, 'greet' being exported, is
// trusted(caller-contract). The merged spatial facet is checked. The null facet of 'name'
// is checked too; __weavec_strnlen itself traps on null with the nonnull template. The body
// stays on one line, as the RFC writes it. The runs pass a 20-byte name and no name.
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAA
// RUN-INPUT:
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void greet(const char *name) { char buf[16]; strcpy(buf, name); puts(buf); } // TRAP: len // TRAP: nonnull // TRUSTED: spatial:caller-contract

int main(int argc, char **argv) {
  greet(argc > 1 ? argv[1] : NULL);
  return 0;
}
