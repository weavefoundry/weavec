/* RFC 0028: independent regression beyond the frozen population. */
#include "local-static.h"
void local_destroy(struct node *p,unsigned value) { static unsigned enabled; enabled=value; if (enabled) destroy(p); }
