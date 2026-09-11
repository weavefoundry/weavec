#include <stdlib.h>
struct item { char *data; struct item *link; };
static void clear(struct item *p) {
  while(p) { struct item *next=p->link; free(p->data); free(p); p=next; }
}
int main(void) { char data=0; struct item *p=malloc(sizeof *p); if(!p) return 0; p->data=&data; p->link=NULL; clear(p); return 0; }
