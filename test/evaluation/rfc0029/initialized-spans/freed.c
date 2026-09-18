#include <stdlib.h>
int pair(const unsigned char *, const unsigned char *);
int main(void) { unsigned char *a=malloc(2); if(!a)return 0; a[0]=1;a[1]=2; free(a); return pair(a,a+2); }
