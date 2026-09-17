#include <stdlib.h>
#include <string.h>
#include <stddef.h>
void copy(char *p,char **out){*out=p;}
int main(void){char *p=malloc(3);if(!p)return 0;p[0]=1;char *q=0;copy(p,&q);return q[0];}
