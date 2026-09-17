#include <stdlib.h>
#include <string.h>
struct node {struct node *child;int kind;};
struct writer {unsigned char *data;size_t cap,len,depth;int flags;};
static int emit(const struct node *n,struct writer *w) {
 if(n->kind==64) {
  if(w->len>w->cap || w->cap-w->len<3)return 0;
  unsigned char *p=w->data+w->len;
  p[0]='{';p[1]='}';p[2]=0;w->len+=2;return 1;
 }
 return emit(n->child,w);
}
int main(void){
 struct node n={0,64};struct writer w[1];memset(w,0,sizeof w);
 w->data=malloc(256);w->cap=256;if(!w->data)return 0;
 if(!emit(&n,w)){free(w->data);return 0;}
 int ok=strlen((char*)w->data)==2;
 free(w->data);return !ok;
}
