#include <stdlib.h>
#include <string.h>
struct node {struct node *next,*child;unsigned value;};
unsigned inspect(struct node *,const char *);
unsigned read_tree(const struct node *);
void drop(struct node *);
