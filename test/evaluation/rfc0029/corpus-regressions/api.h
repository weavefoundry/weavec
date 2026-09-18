#include <stddef.h>
struct node { struct node *next, *child; unsigned value; };
void destroy(struct node *p);
struct node *at(const struct node *array, size_t item);
