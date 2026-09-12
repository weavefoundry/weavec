/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){FILE *f=tmpfile();if(!f)return 0;fclose(f);return fputs("ok",f);}
