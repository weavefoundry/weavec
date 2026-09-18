#include <stdlib.h>
struct reader { size_t depth; const unsigned char *data; size_t remaining; };
struct node { unsigned char value; struct node *next; };
static void destroy(struct node *p) { if(p){destroy(p->next);free(p);} }
struct node *build(struct reader r) {
    if (!r.remaining) return 0;
    struct node *p=calloc(1,sizeof *p); if(!p)return 0;
    p->value=r.data[0];
    if(r.remaining>1){
        r.data++; r.remaining--;
        p->next=build(r);
        if(!p->next){free(p);return 0;}
    }
    return p;
}
int main(void) { const unsigned char data[]={1,2,3}; struct reader r={42,data,3}; struct node *p=build(r); destroy(p); return r.remaining!=3; }
