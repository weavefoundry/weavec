#include <string.h>
struct row { char *data; unsigned count; };
int plain(void) { struct row r; int a[1]={7}; memset(&r,0,sizeof r); return a[r.count]; }
