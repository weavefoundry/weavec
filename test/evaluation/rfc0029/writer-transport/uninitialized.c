#include "api.h"
int main(void){unsigned char input[3],output[3];input[0]=1;struct writer w={7,output,0,3};(void)emit(input,3,&w);if(w.used)return w.data[w.used-1];return 0;}
