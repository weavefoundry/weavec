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
int main(void){struct state s={0,1,0};return walk(&s);}
