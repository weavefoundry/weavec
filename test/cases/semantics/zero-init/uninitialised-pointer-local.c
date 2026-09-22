// RFC 0030 §11: an uninitialised pointer local is null in the enforcing modes, so its use traps.
// STAGE: S5
// 'p' is assigned on some paths only. Zero-initialisation (-ftrivial-auto-var-init=zero)
// makes it null on the others, and the dereference's null facet is checked, so the run,
// which assigns nothing, traps with nonnull instead of reading through garbage. Some path
// initialises 'p', so there is no use-of-uninitialized error.
// RUN-INPUT:
int main(int argc, char **argv) {
  static int x = 5;
  int *p;
  (void)argv;
  if (argc > 1) p = &x;
  return *p; // TRAP: nonnull
}
