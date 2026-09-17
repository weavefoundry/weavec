
#define NULL ((void *)0)
typedef __SIZE_TYPE__ size_t;
void *malloc(size_t);
void free(void *);
void *memset(void *, int, size_t);
struct hooks { void *(*allocate)(size_t); void (*deallocate)(void*); };
static struct hooks global_hooks = {malloc,free};
struct node { struct node *next,*prev,*child; int flags; char *text,*key; };
static void reset(struct hooks *h){if(!h){global_hooks.allocate=malloc;global_hooks.deallocate=free;return;}global_hooks=*h;}
static struct node *new_item(const struct hooks *h){struct node *p=(struct node*)h->allocate(sizeof(struct node));if(p)memset(p,0,sizeof *p);return p;}
static struct node *create(void){struct node *p=new_item(&global_hooks);if(p)p->flags=1;return p;}
static void destroy(struct node *p){struct node *next=0;while(p){next=p->next;if(!(p->flags&256)&&p->child)destroy(p->child);if(!(p->flags&256)&&p->text){global_hooks.deallocate(p->text);p->text=0;}if(!(p->flags&512)&&p->key){global_hooks.deallocate(p->key);p->key=0;}global_hooks.deallocate(p);p=next;}}
static void suffix(struct node *last,struct node *item){last->next=item;item->prev=last;}
static int add(struct node *array,struct node *item){struct node *child=0;if(!item||!array||array==item)return 0;child=array->child;if(!child){array->child=item;item->prev=item;item->next=0;}else{if(child->prev){suffix(child->prev,item);array->child->prev=item;}}return 1;}
