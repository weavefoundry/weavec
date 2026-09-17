#include <stdlib.h>
struct node {struct node *next,*child;char *text;unsigned flags;};
int inspect(struct node *);
int forward(struct node *);
void drop(struct node *);
