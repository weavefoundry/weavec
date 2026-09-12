/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char b[4];FILE *f=fopen("x","r");if(!f)return 0;size_t n=fread(b,1,4,f);fclose(f);if(n==0)return 0;return b[0];}
