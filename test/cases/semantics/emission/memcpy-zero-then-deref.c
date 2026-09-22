// RFC 0030 §8.3: a zero-length memcpy from null does not let the optimiser drop a later null check.
// STAGE: S5
// The zero-length memcpy passes its null-if-zero check, and it proves nothing about 'p'.
// Clang does not turn memcpy's 'nonnull' declaration into an IR 'nonnull' argument, and LLVM
// does not take an llvm.memcpy with a non-constant length as proof of non-null, so the
// checked dereference after it still traps at -O2 (the executable twin of the §8.3
// rewrite-oracle test).
// FLAGS: -O2
// RUN-INPUT:
#include <stddef.h>
#include <string.h>

int copy_then_read(int *p, const int *src, size_t n) {
  memcpy(p, src, n * sizeof *p);
  return p[0]; // TRAP: nonnull
}

int main(int argc, char **argv) {
  int buf[2] = {1, 2};
  int *p = argc > 5 ? buf : NULL;
  (void)argv;
  return copy_then_read(p, p, (size_t)argc - 1);
}
