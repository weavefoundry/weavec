struct node{struct node *next,*prev,*child;int flags;char *text,*key;};
struct node *create(void);
void destroy(struct node *);
int attach_wrapper(struct node *,struct node *);
void reset_hooks(void);
