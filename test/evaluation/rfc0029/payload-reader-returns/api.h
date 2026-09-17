#include <stddef.h>
struct Node{struct Node *next,*child;unsigned flags;char *text,*name;};
struct Reader{const unsigned char *content;size_t length,offset,depth;};
int fill(struct Node *,struct Reader *);int wrap(struct Node *,struct Reader *);void drop(struct Node *);
