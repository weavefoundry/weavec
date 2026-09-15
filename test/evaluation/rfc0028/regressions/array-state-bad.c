/* RFC 0028: independent regression beyond the frozen population. */
#include "array-state.h"
int main(void) { array_configure(0); struct node *p=nodes(2); array_destroy(p); }
