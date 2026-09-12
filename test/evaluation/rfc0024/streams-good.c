/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){FILE *f=tmpfile();if(!f)return 0;char b[2]={1,2};fwrite(b,1,2,f);fputs("ok",f);fputc(10,f);fflush(f);fclose(f);puts("ok");putchar(10);return 0;}
