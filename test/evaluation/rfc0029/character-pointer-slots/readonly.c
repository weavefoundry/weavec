#include <stdlib.h>
int main(void) { unsigned char text[]="12"; unsigned char *const end=0; (void)strtod((const char *)text,(char **)&end); return 0; }
