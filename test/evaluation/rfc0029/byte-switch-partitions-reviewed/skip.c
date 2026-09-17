#include "api.h"
int main(void){const unsigned char p[]="1x2x";size_t k=scan(p,sizeof p,0);unsigned char out[1]={0};if(k==1)return 0;return out[k+1];}
