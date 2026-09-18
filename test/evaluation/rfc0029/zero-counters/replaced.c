#include <string.h>
struct row { char *data; unsigned count; };
int replaced(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); r.count=1; return a[r.count]; }
