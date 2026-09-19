// RFC 0030 §11: a pointer local whose declaration a jump bypasses is not zero-initialised.
// STAGE: S5
// 'p' is declared in the switch body before the first case, so every jump to a case bypasses
// its declaration and -ftrivial-auto-var-init=zero does not initialise it. Its dereference
// has null facet unresolved(no-zero-init), and the unit's A5 summary counts one bypassed
// declaration.
// EXPECT-LEDGER: /units/0/summary/a5/bypassedDeclarations == 1
int pick(int k, int *q) {
  switch (k) {
    int *p;
  case 0:
    p = q;
    /* fall through */
  case 1:
    return *p; // UNRESOLVED: null:no-zero-init
  default:
    return 0;
  }
}
