#include "api.h"
int main(void){unsigned char input[]={1,2,3},output[3];struct writer w={7,output,0,3};(void)emit(input,4,&w);if(w.used)return w.data[w.used-1];return 0;}
