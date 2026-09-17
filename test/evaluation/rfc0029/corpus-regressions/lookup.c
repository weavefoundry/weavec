#include "api.h"
void clear(struct node *p) { if(p)destroy(p->child); }
struct node *at(const struct node *array, size_t item) {
 struct node *child=array?array->child:0;
 while(child && item>0){--item;child=child->next;}
 return child;
}
int main(void) { struct node child={0,0,7}, root={0,&child,0};
 struct node *p=at(&root,0);return p?(int)p->value:0;}
