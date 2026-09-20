// RFC 0030, section 10.6, gate G8: a check inside a GNU statement expression and a _Generic selection.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-statement-expression.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-statement-expression.expected.c %t

int stmt(int *p) { return ({ int v = *p; v + 1; }); }
int gen(int *p) { return _Generic(p, int *: *p, default: 0); }
