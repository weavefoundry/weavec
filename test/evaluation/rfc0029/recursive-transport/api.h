#include <stddef.h>
struct node { unsigned char value; struct node *next; };
struct node *build(const unsigned char *, size_t);
unsigned sum(const struct node *);
void destroy(struct node *);
