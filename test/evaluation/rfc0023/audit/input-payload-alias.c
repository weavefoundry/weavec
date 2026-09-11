// RFC 0023: supplemental quantified-release alias audit.
#include <stdlib.h>
struct item { char *data; struct item *link; };
static void clear(struct item *p) {
  while(p) { struct item *next=p->link; free(p->data); free(p); p=next; }
}
unsigned client(struct item *p, char *alias) {
 if(alias) { unsigned value=(unsigned char)*alias;
  clear(p); return value+(unsigned char)*alias; }
 return 0;
}
