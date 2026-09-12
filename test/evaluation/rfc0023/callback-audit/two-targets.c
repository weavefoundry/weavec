// RFC 0023: quantified consumption through resolved callbacks.
#include <stdlib.h>
struct item { char *data; struct item *link; };
static void clear(struct item *p) {
 while(p) { struct item *next=p->link; free(p->data); free(p); p=next; }
}
static void clear2(struct item *p) { clear(p); }
static void head(struct item *p) { if(p) { free(p->data); free(p); } }
unsigned client(struct item *p, int n) {
 if(p && p->link) { char *saved=p->link->data;
  if(saved) { unsigned value=(unsigned char)*saved;
   void (*release)(struct item *)=n?clear:clear2; release(p);
   return value+(unsigned char)*saved;
  }
 }
 return 0;
}
