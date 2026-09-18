#include "api.h"
int main(void){unsigned char p[]="1x2x";p[1]='1';size_t k=scan(p,sizeof p,0);unsigned char out[1]={0};if(k==1)return 0;return out[k+1];}
