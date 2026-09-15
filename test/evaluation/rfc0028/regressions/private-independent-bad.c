/* RFC 0028: independent regression beyond the frozen population. */
#include "modules.h"
int main(void) { a_configure(1); b_configure(0); struct node *p=nodes(2); b_destroy(p); }
