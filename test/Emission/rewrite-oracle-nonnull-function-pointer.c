// RFC 0030, section 10.6, gate G8: the function-pointer form wraps the callee of an indirect call.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-function-pointer.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-function-pointer.expected.c %t

struct ops { int (*cb)(int); };
int apply(int (*f)(int), int x) { return f(x) + (*f)(x); }
int run(struct ops *o, int x) { return o->cb(x); }
