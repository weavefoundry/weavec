#include "api.h"
int main(void){struct node *n=calloc(1,sizeof *n);if(!n)return 0;n->text=malloc(4);if(!n->text){drop(n);return 0;}if(inspect(n))drop(n);else drop(n);return 0;}
