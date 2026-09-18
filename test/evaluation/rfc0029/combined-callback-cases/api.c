#include "api.h"
int invoke(int enabled, writer_fn write, unsigned char *p) {
 if(!enabled)return 0; write(p); return p[0];
}
