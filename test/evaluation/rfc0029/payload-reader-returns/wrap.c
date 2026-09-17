#include "api.h"
int wrap(struct Node*n,struct Reader*r){if(!fill(n,r))return 0;r->offset++;return 1;}
