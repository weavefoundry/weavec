/* RFC 0028: independent regression beyond the frozen population. */
#include "array-state.h"
static struct { unsigned values[2]; } settings;
void array_configure(unsigned n) { settings.values[1]=n; }
void array_destroy(struct node *p) { if (settings.values[1]) destroy(p); }
