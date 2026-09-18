#include "api.h"
static void fill(unsigned char *p) {p[0]=7;}
int main(void) {unsigned char p[1];return invoke(1,fill,p)!=7;}
