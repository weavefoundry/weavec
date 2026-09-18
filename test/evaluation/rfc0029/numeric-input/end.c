#include <stdlib.h>
int main(void){char text[]="12.5x";char *end=0;double x=strtod(text,&end);(void)x;return *end==0;}
