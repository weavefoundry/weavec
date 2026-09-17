struct node {struct node *next;unsigned tag;};
struct state {unsigned depth,flag;};
static int walk(struct node *p,struct state *s) {
 if (!p) return 0;
 if (s->depth>=1000) return 0;
 if (!s->flag) ++s->depth;
 if (p->tag==1) return 1;
 if (p->next) return walk(p->next,s);
 return 0;
}
int generic(struct node *p) {struct state s={0,0};return walk(p,&s);}
int main(void){struct node n={0,1};return walk(&n,0);}
