#include <stdlib.h>
struct node { unsigned value; struct node *next; };
int main(void) { struct node *p=NULL; return p->value; }
