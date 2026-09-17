#include <stdlib.h>
#include "api.h"
void destroy(struct node *p) { if(p){destroy(p->next);free(p);} }
struct node *build(const unsigned char *data, size_t n) {
    if (!n) return 0;
    struct node *p=calloc(1,sizeof *p);
    if (!p) return 0;
    p->value=data[0];
    if (n>1) {
      p->next=build(data+1,n-1);
      if (!p->next) {free(p);return 0;}
    }
    return p;
}
unsigned sum(const struct node *p) { if (!p) return 0; return p->value + sum(p->next); }
