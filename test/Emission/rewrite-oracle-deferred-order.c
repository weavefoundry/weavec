// RFC 0030, section 10.6, gate G8: functions used before their definition, static and inline ones all get their checks.
// The -O0 IR equals that of Inputs/rewrite-oracle-deferred-order.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-deferred-order.expected.c %t

static int later(int *p);
int early(int *p) { return later(p); }
static int later(int *p) { return *p + 1; }
static inline int helper(int *p) { return *p; }
int use(int *p) { return helper(p); }
