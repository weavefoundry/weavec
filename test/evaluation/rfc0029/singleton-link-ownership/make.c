#include "api.h"
struct node *make(void){struct node *p=malloc(sizeof *p);if(p){p->next=0;p->child=0;p->flags=0;}return p;}
