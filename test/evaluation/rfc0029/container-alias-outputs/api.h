#include <stdlib.h>
struct node{struct node *next;char *payload;unsigned flags;};
void update(struct node*);void forward(struct node*);void drop(struct node*);
