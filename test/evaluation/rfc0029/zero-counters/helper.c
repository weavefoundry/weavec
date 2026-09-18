#include <string.h>
struct row { char *data; unsigned count; };
static void change(struct row *r) { r->count=1; }
int helper(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); change(&r); return a[r.count]; }
