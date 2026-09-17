#include <stdlib.h>
struct node {struct node *next,*prev,*child;unsigned flags;};
struct reader {const unsigned char *data;size_t length,offset;};
struct node *make(void);void drop(struct node*);void mark(struct node*);void forward_mark(struct node*);int build(struct node*,struct reader*);
