#include "api.h"
#include <stdlib.h>
int role(struct Reader*r){if(r->offset<r->length){r->offset++;return 1;}return 0;}
int fill(struct Node*n,struct Reader*r){char*p=malloc(4);if(!p)goto fail;p[0]=0;n->text=p;r->offset++;return 1;fail:free(n);r->offset=1;return 0;}
