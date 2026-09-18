#include <stdlib.h>
struct node { struct node *next,*child; };
static void drop(struct node*n){while(n){struct node*next=n->next;drop(n->child);free(n);n=next;}}
static int grow(struct node*n,unsigned depth){if(!depth)return 1;struct node*c=calloc(1,sizeof *c);if(!c)return 0;if(!grow(c,depth-1)){drop(c); return 0;}n->child=c;n->next=c; return 1;}
int main(int argc,char**argv){(void)argv;struct node*n=calloc(1,sizeof *n);if(!n)return 0;(void)grow(n,(unsigned)argc);drop(n);return 0;}
