// Salvaged false proof (RFC 0030 section 17.2): rfc0029/attached-payload-transfer, case ignored (candidates 99-102).
// The result of attach_wrapper() is ignored, so i leaks when attach fails.
// UNITS: Inputs/attached-payload-library.c
#include "Inputs/attached-payload-api.h"
int main(void){reset_hooks();struct node *o=create();if(!o)return 0;struct node *i=create();if(!i){destroy(o);return 0;}
attach_wrapper(o,i); // MISS: i leaks when attach fails; a leak is not a facet and v0.10.0 is silent
destroy(o);return 0;}
