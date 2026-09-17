#include "api.h"
int main(void){struct node *p=calloc(1,sizeof *p);if(!p)return 0;p->payload=malloc(1);if(!p->payload){drop(p);return 0;} struct node *a=p;a->payload=0;update(a); drop(p);return 0;}
