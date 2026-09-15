#include "tree.h"
int main(int argc,char **argv) {(void)argv;unsigned n=(unsigned)argc;if(n>10000)return 0;struct node *root=0;for(unsigned i=0;i<n;++i){struct node *p=malloc(sizeof *p);if(!p){destroy(root);return 0;}*p=(struct node){i,root,0};root=p;}destroy(root);return 0;}
