// RFC 0030 §2.1: unevaluated operands, '&*p' and the offsetof idiom create no site.
// STAGE: S3
// 'sizeof *p' and 'sizeof p[3]' are unevaluated, '&*p' creates no site, and
// '&((struct s *)0)->b' is the old offsetof idiom on a null pointer constant. The stores go
// to globals without a declared kind, which are no sites either, so each function's only
// site is its exit at the end of the body: the unit has three sites.
// CLEAN
// EXPECT-LEDGER: /summary/sites == 3
#include <stddef.h>

struct s { int a; int b; };

size_t width_sum;
int *same_pointer;
size_t offset_of_b;

void widths(int *p) { width_sum = sizeof *p + sizeof p[3]; }

void same(int *p) { same_pointer = &*p; }

void offset_b(void) { offset_of_b = (size_t)&((struct s *)0)->b; }
