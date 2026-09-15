#include "tree.h"
int main(void) {struct node a={1,0,0},b={2,0,0},p={3,&a,&b};return total(&p)!=6;}
