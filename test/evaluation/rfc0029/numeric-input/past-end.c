#include <stdlib.h>
int main(void){char text[]="12";char *end=0;(void)strtod(text,&end);return end[3];}
