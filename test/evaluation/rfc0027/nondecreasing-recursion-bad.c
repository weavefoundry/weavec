#include <stdlib.h>
struct node {unsigned n;struct node *left,*right;};
static void destroy(struct node *p){if(!p)return;destroy(p);free(p);}
