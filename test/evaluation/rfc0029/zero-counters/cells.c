#include <string.h>
struct row { char *data; unsigned count; };
int cells(void) { struct row r[2]; int a[1]={7}; memset(r,0,sizeof r); r[1].count=1; return a[r[1].count]; }
