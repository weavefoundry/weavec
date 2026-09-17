struct node {struct node *next;unsigned tag;};
struct state {unsigned seen;};
static int walk(struct node *p,struct state *s) {
 if(!p)return 0;
 s->seen=1;
 if(p->tag==1)return 1;
 return walk(p->next,s);
}
int generic_0(struct node *p){struct state s={0};return walk(p,&s);}
int generic_1(struct node *p){struct state s={1};return walk(p,&s);}
int generic_2(struct node *p){struct state s={2};return walk(p,&s);}
int generic_3(struct node *p){struct state s={3};return walk(p,&s);}
int generic_4(struct node *p){struct state s={4};return walk(p,&s);}
int generic_5(struct node *p){struct state s={5};return walk(p,&s);}
int generic_6(struct node *p){struct state s={6};return walk(p,&s);}
int generic_7(struct node *p){struct state s={7};return walk(p,&s);}
int generic_8(struct node *p){struct state s={8};return walk(p,&s);}
int generic_9(struct node *p){struct state s={9};return walk(p,&s);}
int generic_10(struct node *p){struct state s={10};return walk(p,&s);}
int generic_11(struct node *p){struct state s={11};return walk(p,&s);}
int generic_12(struct node *p){struct state s={12};return walk(p,&s);}
int generic_13(struct node *p){struct state s={13};return walk(p,&s);}
int generic_14(struct node *p){struct state s={14};return walk(p,&s);}
int generic_15(struct node *p){struct state s={15};return walk(p,&s);}
int generic_16(struct node *p){struct state s={16};return walk(p,&s);}
int generic_17(struct node *p){struct state s={17};return walk(p,&s);}
int generic_18(struct node *p){struct state s={18};return walk(p,&s);}
int generic_19(struct node *p){struct state s={19};return walk(p,&s);}
int generic_20(struct node *p){struct state s={20};return walk(p,&s);}
int generic_21(struct node *p){struct state s={21};return walk(p,&s);}
int generic_22(struct node *p){struct state s={22};return walk(p,&s);}
int generic_23(struct node *p){struct state s={23};return walk(p,&s);}
int generic_24(struct node *p){struct state s={24};return walk(p,&s);}
int generic_25(struct node *p){struct state s={25};return walk(p,&s);}
int generic_26(struct node *p){struct state s={26};return walk(p,&s);}
int generic_27(struct node *p){struct state s={27};return walk(p,&s);}
int generic_28(struct node *p){struct state s={28};return walk(p,&s);}
int generic_29(struct node *p){struct state s={29};return walk(p,&s);}
int generic_30(struct node *p){struct state s={30};return walk(p,&s);}
int generic_31(struct node *p){struct state s={31};return walk(p,&s);}
int generic_32(struct node *p){struct state s={32};return walk(p,&s);}
int generic_33(struct node *p){struct state s={33};return walk(p,&s);}
int generic_34(struct node *p){struct state s={34};return walk(p,&s);}
int generic_35(struct node *p){struct state s={35};return walk(p,&s);}
