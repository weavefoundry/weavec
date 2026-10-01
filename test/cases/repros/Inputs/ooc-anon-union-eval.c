// Second unit of ooc-anon-union-record.c: tinyexpr's te_expr keeps its value or its bound
// variable in an anonymous union ('union { double value; const double *bound; ... };'),
// and eval reads 'n->value' or '*n->bound' by the node type. Its unit record carries a
// summary that reads the anonymous member.
typedef struct node { int type; union { double value; const double *bound; }; } node;
double eval(const node *n) {
  switch (n->type) { case 0: return n->value; default: return *n->bound; }
}
