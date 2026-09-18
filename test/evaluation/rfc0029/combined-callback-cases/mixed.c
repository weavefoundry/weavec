#include "api.h"
static void fill(unsigned char *p) {p[0]=7;}
static void skip(unsigned char *p) {(void)p;}
int main(int argc,char **argv) {(void)argv;unsigned char p[1];return invoke(1,argc>1?fill:skip,p);}
