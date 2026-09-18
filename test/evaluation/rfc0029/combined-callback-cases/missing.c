#include "api.h"
static void skip(unsigned char *p) {(void)p;}
int main(void) {unsigned char p[1];return invoke(1,skip,p);}
