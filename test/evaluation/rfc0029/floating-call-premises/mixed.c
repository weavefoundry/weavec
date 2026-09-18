#include "api.h"
int main(void){volatile int k=0;return clamp(k?1.0:__builtin_nan(""));}
