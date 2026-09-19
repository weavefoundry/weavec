// Salvaged false proof (RFC 0030 section 17.2): rfc0029/attached-payload-transfer, case leak (candidates 99-102).
// When attach() fails (its key allocation returns null), i is never released.
// UNITS: Inputs/attached-payload-library.c
#include "Inputs/attached-payload-api.h"
int main(void){reset_hooks();struct node *o=create();if(!o)return 0;struct node *i=create();if(!i){destroy(o);return 0;}
if(!attach_wrapper(o,i)){destroy(o);return 0;} // MISS: i leaks when attach fails; a leak is not a facet and v0.10.0 is silent
destroy(o);return 0;}
