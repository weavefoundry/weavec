// RFC 0031 *Implementation amendments*: globals that unknown code may write.
// STAGE: S7
// `unknown` may write any global, so after it `g.p` is a new unknown value,
// not the one the function tested at entry: its target is no longer the
// entry object that value pointed to, and nothing is proven about it (the
// cells were forgotten but read their entry values again, a false spatial
// proof; cJSON's tests reset a global item and parse into it, and the
// reread child was the one the reset had freed).
// TOOL
// EXPECT-LEDGER: /summary/facets/spatial/proven == 0
struct S {
  int *p;
};

struct S g;

extern void unknown(void);

int f(void) {
  if (!g.p)
    return 0;
  unknown();
  return *g.p; // NOT-PROVEN: spatial
}
