#include <string.h>
struct row { char *data; unsigned count; };
int array(void) { struct row r[2]; int a[1]={7}; memset(r,0,sizeof r); return a[r[1].count]; }
