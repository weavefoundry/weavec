#ifndef RFC0023_NODE_H
#define RFC0023_NODE_H
struct node { unsigned value; struct node *next; };
struct node *build(unsigned);
unsigned count(const struct node *);
void destroy(struct node *);
struct node *reverse(struct node *);
struct node *concat(struct node *, struct node *);
struct node *pop(struct node **);
#endif
