struct node { struct node *next; unsigned tag; };
struct state { unsigned seen, mode; };
int walk(struct node *p, struct state *s) {
    if (!p) return 0;
    s->seen = s->mode;
    if (p->tag == 1) return 1;
    return walk(p->next, s);
}
