#include <string.h>
struct row { char *data; unsigned count; };
int partial(void) { struct row r; int a[1]={7}; r.count=256; memset(&r.count,0,1); return a[r.count]; }
