#include <stdlib.h>
int main(void) { unsigned char text[]="12"; unsigned char *end=0; (void)strtod((const char *)text,(char **)&end); return end[3]; }
