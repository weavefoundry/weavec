#include <stdlib.h>
struct node { unsigned char value; struct node *next; };
static void destroy(struct node *p) { if(p){destroy(p->next);free(p);} }
int build(const unsigned char *data, size_t n, struct node **out) {
    *out = 0;
    if (!n) return 0;
    struct node *p=calloc(1,sizeof *p);
    if (!p) return 0;
    p->value=data[0];
    if (n>1 && !build(data+1,n-1,&p->next)) {free(p);return 0;}
    *out=p;
    return 1;
}
int main(void) {const unsigned char data[]={1,2,3};struct node *p=0;if(build(data,4,&p))destroy(p);return 0;}
