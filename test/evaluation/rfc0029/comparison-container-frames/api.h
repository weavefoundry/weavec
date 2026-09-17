#include <stdlib.h>
#include <string.h>
struct node {struct node *next,*child;int value;};
int inspect(struct node *,const char *);
int read_tree(const struct node *);
void drop(struct node *);
