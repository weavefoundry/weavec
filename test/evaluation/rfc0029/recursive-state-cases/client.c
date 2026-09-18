struct node { struct node *next; unsigned tag; };
struct state { unsigned seen, mode; };
static int walk(struct node *p, struct state *s) {
    if (!p) return 0;
    s->seen = s->mode;
    if (p->tag == 1) return 1;
    return walk(p->next, s);
}
int generic_0(struct node *p) { struct state s = {0, 0}; return walk(p, &s); }
int generic_1(struct node *p) { struct state s = {0, 1}; return walk(p, &s); }
int generic_2(struct node *p) { struct state s = {0, 2}; return walk(p, &s); }
int generic_3(struct node *p) { struct state s = {0, 3}; return walk(p, &s); }
int generic_4(struct node *p) { struct state s = {0, 4}; return walk(p, &s); }
int generic_5(struct node *p) { struct state s = {0, 5}; return walk(p, &s); }
int generic_6(struct node *p) { struct state s = {0, 6}; return walk(p, &s); }
int generic_7(struct node *p) { struct state s = {0, 7}; return walk(p, &s); }
int generic_8(struct node *p) { struct state s = {0, 8}; return walk(p, &s); }
int generic_9(struct node *p) { struct state s = {0, 9}; return walk(p, &s); }
int generic_10(struct node *p) { struct state s = {0, 10}; return walk(p, &s); }
int generic_11(struct node *p) { struct state s = {0, 11}; return walk(p, &s); }
int generic_12(struct node *p) { struct state s = {0, 12}; return walk(p, &s); }
int generic_13(struct node *p) { struct state s = {0, 13}; return walk(p, &s); }
int generic_14(struct node *p) { struct state s = {0, 14}; return walk(p, &s); }
int generic_15(struct node *p) { struct state s = {0, 15}; return walk(p, &s); }
int generic_16(struct node *p) { struct state s = {0, 16}; return walk(p, &s); }
int generic_17(struct node *p) { struct state s = {0, 17}; return walk(p, &s); }
int generic_18(struct node *p) { struct state s = {0, 18}; return walk(p, &s); }
int generic_19(struct node *p) { struct state s = {0, 19}; return walk(p, &s); }
int generic_20(struct node *p) { struct state s = {0, 20}; return walk(p, &s); }
int generic_21(struct node *p) { struct state s = {0, 21}; return walk(p, &s); }
int generic_22(struct node *p) { struct state s = {0, 22}; return walk(p, &s); }
int generic_23(struct node *p) { struct state s = {0, 23}; return walk(p, &s); }
int generic_24(struct node *p) { struct state s = {0, 24}; return walk(p, &s); }
int generic_25(struct node *p) { struct state s = {0, 25}; return walk(p, &s); }
int generic_26(struct node *p) { struct state s = {0, 26}; return walk(p, &s); }
int generic_27(struct node *p) { struct state s = {0, 27}; return walk(p, &s); }
int generic_28(struct node *p) { struct state s = {0, 28}; return walk(p, &s); }
int generic_29(struct node *p) { struct state s = {0, 29}; return walk(p, &s); }
int generic_30(struct node *p) { struct state s = {0, 30}; return walk(p, &s); }
int generic_31(struct node *p) { struct state s = {0, 31}; return walk(p, &s); }
int generic_32(struct node *p) { struct state s = {0, 32}; return walk(p, &s); }
int generic_33(struct node *p) { struct state s = {0, 33}; return walk(p, &s); }
int generic_34(struct node *p) { struct state s = {0, 34}; return walk(p, &s); }
int generic_35(struct node *p) { struct state s = {0, 35}; return walk(p, &s); }
int main(void) { struct node n = {0, 1}; struct state s = {0, 100}; return walk(&n, &s); }
