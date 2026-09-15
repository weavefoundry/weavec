/* RFC 0028: independent regression beyond the frozen population. */
#include "modules.h"
static unsigned enabled;
void a_configure(unsigned n) { enabled = n; }
void a_destroy(struct node *p) { if (enabled) destroy(p); }
