/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
int main(void) { struct node *p=nodes(2); configure(1); conditional_destroy(p); }
