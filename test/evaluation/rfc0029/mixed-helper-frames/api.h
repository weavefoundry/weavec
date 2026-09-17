#include <stdlib.h>
struct node {struct node *next,*child;unsigned flags;};
struct reader {unsigned position,capacity,extra;};
struct node *make(void);void drop(struct node*);void build(struct node*,struct reader*);void step(struct reader*);
