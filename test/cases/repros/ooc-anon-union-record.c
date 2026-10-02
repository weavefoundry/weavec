// Held-out repro (RFC 0031 Motivation, §7, §11.1): tinyexpr (tinyexpr.c te_eval) reads a
// member of an anonymous union inside 'te_expr'. v0.11.0 writes a unit record whose summary
// of that function does not decode at link ("payload.functions[N].summary: invalid object
// view"), so the link step drops the unit with "link input 'tinyexpr.o' has a stale WeaveC
// record ...; calls into it are trusted" [weavec::unanalyzed-input].
// Reduced from build/rfc31/ooc/repro/anon.c and anon_main.c: this unit calls 'eval' in
// Inputs/ooc-anon-union-eval.c, and both are analysed and linked.
// intended: no finding; the record of a function reading an anonymous member round-trips.
// UNITS: Inputs/ooc-anon-union-eval.c
// CLEAN
// ASAN
typedef struct node { int type; union { double value; const double *bound; }; } node;
double eval(const node *n);
int main(void) {
  node n = {0};
  n.value = 1;
  double x = 2;
  node m = {1, {0}};
  m.bound = &x;
  return (int)eval(&n) + (int)eval(&m) == 3 ? 0 : 1;
}
