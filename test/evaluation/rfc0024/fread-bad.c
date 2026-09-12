/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char b[4];FILE *f=fopen("x","r");if(!f)return 0;fread(b,1,1,f);fclose(f);return b[3];}
