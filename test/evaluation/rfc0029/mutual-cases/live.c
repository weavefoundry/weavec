struct state {unsigned depth,tag;char *data;};
static int next(struct state *s);
static int walk(struct state *s) {
 if (!s->tag) return 0;
 if (s->depth>=1000) return 0;
 ++s->depth;
 return next(s);
}
static int next(struct state *s) {
 if (*s->data==1) return 100 / *s->data;
 return walk(s);
}
int main(void){char data=1;struct state s={0,1,&data};return walk(&s);}
