#include <string.h>
struct row { char *data; unsigned count; };
int plain(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); return a[r.count]; }
int array(void) { struct row r[2]; int a[1]={7}; memset(r,0,sizeof r); return a[r[1].count]; }
int partial(void) { struct row r; int a[1]={7}; r.count=256; memset(&r.count,0,1); return a[r.count]; }
int replaced(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); r.count=1; return a[r.count]; }
static void change(struct row *r) { r->count=1; }
int helper(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); change(&r); return a[r.count]; }
int cells(void) { struct row r[2]; int a[1]={7}; memset(r,0,sizeof r); r[1].count=1; return a[r[1].count]; }
