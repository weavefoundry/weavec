// RFC 0031 *Implementation amendments*: nullness through arithmetic.
// STAGE: S7
// A pointer made by arithmetic from another is null exactly when that one
// is (arithmetic on null is undefined), so the check that `*(q++)` makes on
// `q` decides the incremented `q` too: only the first of the fetches is
// checked (and `f->u`), as each fetch of an interpreter's `pc` follows another (Lua's
// `vmfetch`). The pointer here comes from an untyped union cell, whose value
// the check does not give a type.
// TOOL
// EXPECT-LEDGER: /summary/facets/null/checked == 3
// EXPECT-LEDGER: /summary/facets/null/proven == 3
union cell {
  const int *code;
  void (*hook)(void);
};

struct frame {
  union cell u;
};

int sum3(const int *q) {
  int a = *(q++);
  int b = *(q++);
  int c = *(q++);
  return a + b + c;
}

int fetch2(struct frame *f) {
  const int *pc = f->u.code;
  int a = *(pc++);
  int b = *(pc++);
  return a + b;
}
