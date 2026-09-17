struct Node {struct Node *next,*child;unsigned flags;char *text,*name;};
int fill(struct Node *,unsigned *);void drop(struct Node *);
