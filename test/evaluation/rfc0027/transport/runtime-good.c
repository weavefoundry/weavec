#include "tree-api.h"
int main(int argc,char **argv) {(void)argv;unsigned n=(unsigned)argc;if(n>10000)return 0;struct node *p=make(n);unsigned v=total(p);destroy(p);return v==42;}
