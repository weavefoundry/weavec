#include <stdlib.h>
struct node {struct node *next,*child;char *text;unsigned value;};
unsigned inspect(struct node *,const char *);
unsigned read_tree(const struct node *);
void drop(struct node *);
