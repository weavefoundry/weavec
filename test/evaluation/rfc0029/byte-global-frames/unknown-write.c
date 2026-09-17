#include "api.h"
int main(int argc,char **argv){unsigned char text[]={'a','b','c'};if(argc>0&&argc<3)text[argc]='\\';return inspect(text,3);}
