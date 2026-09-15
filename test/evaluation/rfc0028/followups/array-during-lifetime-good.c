/* RFC 0028: a disjoint private array write preserves an object. */
#include "../regressions/array-state.h"
int main(void) { array_configure(0); struct node *p=nodes(2); array_configure(1); array_destroy(p); }
