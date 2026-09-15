/* RFC 0027: interface for the frozen tree helper bodies. */
#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
unsigned total(const struct node *p);
void destroy(struct node *p);
struct node *make(unsigned n);
struct node *detach(struct node *p);
void attach(struct node *p, struct node *child);
