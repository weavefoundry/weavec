#include "api.h"
void mark(struct node *p){p->flags=2;}
void forward_mark(struct node *p){mark(p);}
