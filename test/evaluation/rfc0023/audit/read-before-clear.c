// RFC 0023: supplemental quantified-release alias audit.
#include <stdlib.h>
struct item { char *data; struct item *link; };
static void clear(struct item *p) {
  while(p) { struct item *next=p->link; free(p->data); free(p); p=next; }
}
unsigned client(struct item *p) {
 if(p && p->link) { char *saved=p->link->data;
  if(saved) { unsigned value=(unsigned char)*saved;
   clear(p); return value; } }
 return 0;
}
