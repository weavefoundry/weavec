#include "api.h"
int main(void){char input[3];input[0]=104;input[2]=0;char *p=duplicate(input);if(p)free(p);return 0;}
