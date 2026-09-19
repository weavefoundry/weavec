// RFC 0030 §2.6: a C99 inline definition inlined at -O2 is analysed and instrumented.
// STAGE: S5
// 'get' is a C99 inline definition: CodeGen emits it available_externally and inlines it at
// -O1 and above, so SiteCollector counts it as emitted and CheckEmitter instruments it,
// although this unit emits no external definition. The early return keeps p[i] from being
// a must-access, so no call-site requirement moves its check: the dereference's null facet
// is checked in the body, and the check inlined into 'main' at -O2 traps. The external
// definition, used only by a call that is not inlined, is in the second unit.
// FLAGS: -O2
// UNITS: Inputs/inline-get-extern.c
// RUN-INPUT:
inline int get(const int *p, int i) {
  if (i < 0) return -1;
  return p[i]; // TRAP: nonnull
}

int main(int argc, char **argv) {
  static const int xs[2] = {1, 2};
  (void)argv;
  return get(argc > 1 ? xs : 0, 0);
}
