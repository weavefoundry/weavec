#include <stddef.h>
struct node { unsigned char value; struct node *next; };
int build(const unsigned char *, size_t, struct node **);
void destroy(struct node *);
unsigned sum(const struct node *);
