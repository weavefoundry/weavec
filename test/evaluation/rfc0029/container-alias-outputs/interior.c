#include "api.h"
int main(void){struct node *p=calloc(1,sizeof *p);if(!p)return 0; struct node *a=p;a=(struct node*)((char*)a+1);update(a); drop(p);return 0;}
