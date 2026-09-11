// RFC 0023: independently executable counterexample to the pre-fix proof.
#include "two-targets.c"
int main(void) {
 struct item *b=malloc(sizeof *b), *a=malloc(sizeof *a);
 if(!a || !b) { free(a); free(b); return 0; }
 char *x=malloc(1), *y=malloc(1);
 if(!x || !y) { free(x); free(y); free(a); free(b); return 0; }
 *x=1; *y=2; b->data=y; b->link=0; a->data=x; a->link=b;
 return (int)client(a,1);
}
