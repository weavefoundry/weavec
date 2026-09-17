#include "api.h"
int main(void){static unsigned char text[]={'a','b','c'};text[1]='\\';return inspect(text,3);}
